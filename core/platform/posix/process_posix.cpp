// macOS／Linux：子行程以 --ipc 3 4 取得管道（posix_spawn 把兩端接到 fd 3、4）。
// 父行程結束（包括當掉）時管道關閉，子行程讀到結尾就自行結束。
// 注意：尚未在 macOS 上編譯驗證。
#include "../process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace platform {

namespace {

// 子行程裡管道的 fd
constexpr int kChildIn = 3;
constexpr int kChildOut = 4;

bool ReadFd(int fd, void* buf, size_t size) {
  char* p = static_cast<char*>(buf);
  while (size > 0) {
    const ssize_t got = ::read(fd, p, size);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    p += got;
    size -= (size_t)got;
  }
  return true;
}

bool WriteFd(int fd, const void* buf, size_t size) {
  const char* p = static_cast<const char*>(buf);
  while (size > 0) {
    const ssize_t put = ::write(fd, p, size);
    if (put < 0 && errno == EINTR)
      continue;
    if (put <= 0)
      return false;
    p += put;
    size -= (size_t)put;
  }
  return true;
}

// 對方已結束時寫入管道不要讓整個行程收到 SIGPIPE
void NoSigPipe(int fd) {
#ifdef F_SETNOSIGPIPE
  fcntl(fd, F_SETNOSIGPIPE, 1);
#else
  (void)fd;
  static std::once_flag once;
  std::call_once(once, [] { signal(SIGPIPE, SIG_IGN); });
#endif
}

// 把 fd 移到 10 以上並設 close-on-exec：子行程只拿到 file actions 指定的 fd，
// 也避免 dup2 到 3、4 時蓋掉另一端
int MoveHigh(int fd) {
  const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
  ::close(fd);
  return moved;
}

class FdPipe : public Pipe {
 public:
  FdPipe(int in, int out) : in_(in), out_(out) { NoSigPipe(out_); }
  ~FdPipe() override {
    ::close(in_);
    ::close(out_);
  }
  bool Read(void* buf, size_t size) override { return ReadFd(in_, buf, size); }
  bool Write(const void* buf, size_t size) override { return WriteFd(out_, buf, size); }

 private:
  int in_;
  int out_;
};

}  // namespace

struct ChildProcess::Handles {
  pid_t pid = 0;
  int read = -1;   // 讀子行程的輸出
  int write = -1;  // 寫給子行程
  mutable std::mutex mutex;  // 保護 reaped（Alive 可能和 Shutdown 在不同執行緒）
  mutable bool reaped = false;

  // 子行程已結束就回收；回傳是否已結束
  bool Reap(bool block) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (reaped)
      return true;
    int status = 0;
    pid_t r;
    do {
      r = waitpid(pid, &status, block ? 0 : WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == pid || (r < 0 && errno == ECHILD))
      reaped = true;
    return reaped;
  }
};

ChildProcess::~ChildProcess() {
  if (!h_)
    return;
  Shutdown();
  ::close(h_->read);
}

void ChildProcess::Shutdown() {
  if (h_->write < 0)
    return;
  // 先關掉寫入端：子行程讀到結尾就會自行結束；稍等一下，還沒結束就強制結束
  ::close(h_->write);
  h_->write = -1;
  const auto wait_exit = [this](int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!h_->Reap(false)) {
      if (std::chrono::steady_clock::now() > deadline)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  };
  if (!wait_exit(500)) {
    kill(h_->pid, SIGKILL);
    if (!wait_exit(2000))
      h_->Reap(true);
  }
}

std::unique_ptr<ChildProcess> ChildProcess::Start(const std::filesystem::path& exe,
                                                  const std::vector<std::string>& args) {
  int to_child[2], from_child[2];
  if (pipe(to_child) != 0)
    return nullptr;
  if (pipe(from_child) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    return nullptr;
  }
  const int child_in = MoveHigh(to_child[0]);
  const int parent_write = MoveHigh(to_child[1]);
  const int parent_read = MoveHigh(from_child[0]);
  const int child_out = MoveHigh(from_child[1]);
  auto close_all = [&] {
    for (int fd : {child_in, parent_write, parent_read, child_out})
      if (fd >= 0)
        ::close(fd);
  };
  if (child_in < 0 || parent_write < 0 || parent_read < 0 || child_out < 0) {
    close_all();
    return nullptr;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  // 標準輸入輸出接到空裝置：程式庫印到 stdout 的東西不會混進管道
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, child_in, kChildIn);
  posix_spawn_file_actions_adddup2(&actions, child_out, kChildOut);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  // macOS：只有 file actions 指定的 fd 傳給子行程（其他推理行程的管道不會跟著過去）
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

  const std::string exe_path = exe.string();
  std::vector<std::string> argv_s = {exe_path, "--ipc", std::to_string(kChildIn),
                                     std::to_string(kChildOut)};
  argv_s.insert(argv_s.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : argv_s)
    argv.push_back(&a[0]);
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe_path.c_str(), &actions, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  // 子行程那一端交出去了，自己這邊關掉，子行程結束時讀取才會收到結尾
  ::close(child_in);
  ::close(child_out);
  if (rc != 0) {
    ::close(parent_write);
    ::close(parent_read);
    return nullptr;
  }
  NoSigPipe(parent_write);

  std::unique_ptr<ChildProcess> child(new ChildProcess());
  child->h_ = std::make_unique<Handles>();
  child->h_->pid = pid;
  child->h_->read = parent_read;
  child->h_->write = parent_write;
  return child;
}

bool ChildProcess::Read(void* buf, size_t size) {
  return ReadFd(h_->read, buf, size);
}

bool ChildProcess::Write(const void* buf, size_t size) {
  return h_->write >= 0 && WriteFd(h_->write, buf, size);
}

void ChildProcess::Kill() {
  if (!h_->Reap(false))
    kill(h_->pid, SIGKILL);
}

bool ChildProcess::Alive() const {
  return !h_->Reap(false);
}

std::unique_ptr<Pipe> OpenParentPipe(int argc, char** argv) {
  for (int i = 1; i + 2 < argc; ++i) {
    if (std::string(argv[i]) == "--ipc") {
      const int in = std::atoi(argv[i + 1]);
      const int out = std::atoi(argv[i + 2]);
      if (in <= 2 || out <= 2 || fcntl(in, F_GETFD) < 0 || fcntl(out, F_GETFD) < 0)
        return nullptr;
      // 推理行程自己再啟動的程式不需要這兩個 fd
      fcntl(in, F_SETFD, FD_CLOEXEC);
      fcntl(out, F_SETFD, FD_CLOEXEC);
      return std::make_unique<FdPipe>(in, out);
    }
  }
  return nullptr;
}

std::filesystem::path ExecutableDir() {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(&path[0], &size) != 0)
    return {};
  path.resize(std::strlen(path.c_str()));
  std::error_code ec;
  const std::filesystem::path real = std::filesystem::canonical(path, ec);
  return (ec ? std::filesystem::path(path) : real).parent_path();
#else
  char path[PATH_MAX] = {0};
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0)
    return {};
  return std::filesystem::path(std::string(path, (size_t)n)).parent_path();
#endif
}

}  // namespace platform
