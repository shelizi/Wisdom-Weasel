#pragma once

// 子行程與管道（LLM 推理行程用）。每個平台各一份實作（platform/win/…、platform/mac/…）。
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace platform {

// 雙向的位元組管道；Read 讀滿 size 才回傳 true，對方關閉或出錯時回傳 false
class Pipe {
 public:
  virtual ~Pipe() = default;
  virtual bool Read(void* buf, size_t size) = 0;
  virtual bool Write(const void* buf, size_t size) = 0;
};

// 子行程：啟動時建立一對管道交給子行程，子行程以 OpenParentPipe 取得另一端。
// 子行程的標準輸入輸出都接到空裝置，程式庫印到 stdout 的東西不會混進管道。
// 物件解構時（或父行程結束時）子行程一併結束。
class ChildProcess : public Pipe {
 public:
  ~ChildProcess() override;

  static std::unique_ptr<ChildProcess> Start(const std::filesystem::path& exe,
                                             const std::vector<std::string>& args);

  bool Read(void* buf, size_t size) override;
  bool Write(const void* buf, size_t size) override;
  // 立即結束子行程；阻塞中的 Read 會隨之回傳 false
  void Kill();
  // 子行程還在執行
  bool Alive() const;
  // 關閉寫入端讓子行程自行結束，稍等仍未結束就強制結束；之後不可再 Write。
  // 解構前若有其他執行緒在 Read，先呼叫這個、等那個執行緒結束，再解構
  void Shutdown();

 private:
  ChildProcess() = default;
  struct Handles;
  std::unique_ptr<Handles> h_;
};

// 子行程端：從命令列參數取得與父行程之間的管道；參數不對時回傳 nullptr
std::unique_ptr<Pipe> OpenParentPipe(int argc, char** argv);

// 這個執行檔所在的資料夾（用來找同一資料夾裡的推理程式）
std::filesystem::path ExecutableDir();

}  // namespace platform
