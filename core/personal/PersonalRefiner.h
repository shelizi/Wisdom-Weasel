#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include <vector>
#include "PersonalLexicon.h"

class LLMLocalChatSession;
struct LLMLocalModelSpec;

// 個人詞庫精煉：
// - 增量精煉：處理累積中的原始紀錄，完成後封存到 archive/，之後重新累積
// - 重新精煉全部：用封存 + 累積中的全部原始紀錄從頭重建統計，再精煉一次
// - 選了模型（本機 llama.cpp 或 OpenAI 相容 API）時，請 LLM 挑出錯字、無意義片段與不一致寫法；
//   沒選時只做統計整理（衰減淘汰）
// - 依 interval_days 在閒置時自動執行（0 = 只手動）
// 狀態寫在 personal/status.txt，給設定程式顯示。
class PersonalRefiner {
 public:
  struct Config {
    double interval_days = 1;
    std::string type;        // "openai"、"llamacpp"；空 = 不用 LLM
    std::string name;        // 模型設定的名稱（顯示用）
    std::string api_url;     // openai
    std::string api_key;
    std::string model;
    std::string model_path;  // llamacpp
    bool instruct = true;
    int n_gpu_layers = 0;
    int n_threads = 4;
    bool disable_thinking = false;  // 思考型模型：關閉思考
    int think_tokens = 2048;        // 開啟思考時的思考長度上限（0 = 不限制）
    // 讓常打的詞影響注音選字排序：匯出 Rime 詞典（terra_pinyin.personal），有變動就重新部署
    bool rime_boost = false;
    std::wstring rime_dict_path;   // 使用者資料夾\terra_pinyin.personal.dict.yaml
    std::wstring essay_path;       // 共用資料夾\essay.txt（原本的詞頻）
    std::function<void()> redeploy;  // 請輸入法重新部署（各平台提供）
    // 順便整理 Rime 的選字記憶（使用者詞典）：請 LLM 挑出學到的錯字錯詞並刪除
    bool clean_rime_memory = true;
    std::string rime_user_dict = "terra_pinyin";
    // 借用輸入法的模型時，使用者在打字就等這麼久再試
    unsigned shared_busy_wait_ms = 20000;
    bool UsesLLM() const {
      return (type == "openai" && !api_url.empty()) || (type == "llamacpp" && !model_path.empty());
    }
  };

  explicit PersonalRefiner(PersonalLexicon* lexicon);
  ~PersonalRefiner();

  void Configure(const Config& config);
  // 使用者詞典被輸入法開著時無法匯出／匯入：由服務端提供，在它的鎖下關掉所有 Rime session
  // （放開使用者詞典）後執行 fn；客戶端之後打字會自動重建 session。無法執行時回傳 false
  using UserDictAccess = std::function<bool(const std::function<void()>& fn)>;
  void SetUserDictAccess(UserDictAccess access) { user_dict_access_ = std::move(access); }
  // 精煉的本機模型和輸入法已載入的是同一個時，借來對話，不再載入一份（由服務端提供）。
  // 不是同一個或放不下回傳 kUnavailable；使用者在打字（或要換模型）時回傳 kBusy，等一下再試
  enum class SharedChatResult { kOk, kFailed, kUnavailable, kBusy };
  using SharedChat = std::function<SharedChatResult(
      const LLMLocalModelSpec& spec, const std::string& system_utf8, const std::string& user_utf8,
      int max_tokens, std::string* output, std::wstring* error)>;
  void SetSharedChat(SharedChat chat) { shared_chat_ = std::move(chat); }
  void Start();  // 啟動排程執行緒
  void Stop();   // 停止並等待進行中的精煉結束
  void RequestStop();  // 只通知停止，不等待（進行中的請求會盡快中斷）

  // 在背景執行精煉；已有一個在跑時回傳 false
  bool RunAsync(bool full);
  bool Running() const { return running_; }

  // 清除所有資料（精煉中時不做事，回傳 false）
  bool Clear();

  // 匯出 Rime 詞典；回傳 -1 失敗、0 內容沒變、1 已更新（不部署）
  int ExportRimeDict();
  // 匯出並在內容有變時重新部署（背景精煉完成後呼叫）
  void ExportRimeDictAndDeploy();

  // 重新寫 status.txt
  void WriteStatus() { WriteStatusAs(running_); }

  // 解析 LLM 的拆解結果「拆解\t原片段\t部分 部分…」：只收 known 裡的片段，
  // 且部分依序接起來要等於原片段
  static std::vector<PersonalLexicon::Split> ParseSplits(
      const std::wstring& content, const std::unordered_set<std::wstring>& known);

 private:
  // 一次精煉用的本機模型：第一次對話時才準備，各段（審查、拆解、選字記憶）共用。
  // 優先借用輸入法已載入的同一個模型，不行才自己載入一次
  struct LocalModel {
    bool try_shared = true;
    bool announced = false;  // 已在開發終端說明用哪一份模型
    std::unique_ptr<LLMLocalChatSession> session;
    LocalModel();
    ~LocalModel();
  };
  bool LocalChat(const Config& config, LocalModel* model, const wchar_t* system,
                 const std::wstring& user, int max_tokens, std::string* output,
                 std::wstring* error);

  void SchedulerLoop();
  void Run(bool full);
  // 呼叫 LLM 精煉（分批審查）；records 用來挑例句。回傳結果說明文字，失敗時 ok = false
  //   一般精煉：只審查還沒審查過的詞（最近出現的優先），一批
  //   重新精煉全部：所有的詞分批審查完
  std::wstring RefineWithLLM(const Config& config, LocalModel* model, bool full,
                             const std::vector<PersonalLexicon::RawRecord>& records,
                             bool* ok);

  // 拆解：請 LLM 把太長的片段（整句）拆成詞或片語，之後分別學習。一般精煉看一批還沒看過的，
  // 重新精煉全部時全部重看。沒有要看的回傳空字串，失敗時 ok = false
  std::wstring SplitWithLLM(const Config& config, LocalModel* model, bool full, bool* ok);

  // 整理 Rime 選字記憶：匯出使用者詞典，請 LLM 挑出錯字錯詞（兩字以上、還沒審查過的），
  // 以匯入負次數的方式刪除。回傳結果說明文字，失敗時 ok = false
  std::wstring RefineRimeMemory(const Config& config, LocalModel* model, bool full, bool* ok);

  // system 為空時用個人詞庫精煉的提示詞
  std::wstring CallRemote(const Config& config, const std::wstring& user, size_t words,
                          size_t examples, bool* ok, const std::wstring& system = L"");
  // 解析並套用一批的結果；回傳這批刪除的詞，merged 累加合併數
  std::vector<std::wstring> ApplyLLMResult(const std::wstring& content, size_t batch_size,
                                           const std::unordered_set<std::wstring>& known,
                                           size_t* merged);
  std::string Method() const;  // 精煉方式（status.txt 顯示用，呼叫時已持有 mutex_）
  void WriteStatusAs(bool running);

  PersonalLexicon* lexicon_;
  UserDictAccess user_dict_access_;
  SharedChat shared_chat_;
  mutable std::mutex mutex_;  // config_、last_result_、排程等待
  std::mutex run_mutex_;      // worker_ 的啟動與結束
  Config config_;
  std::wstring last_result_;
  std::wstring progress_;      // 分批進度（例如「第 3／17 批」），status.txt 顯示用
  size_t rime_words_ = 0;      // 上次匯出給 Rime 的詞數
  int64_t rime_updated_ = 0;   // 上次匯出內容有變的時間
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::condition_variable cv_;
  std::thread scheduler_;
  std::thread worker_;
  std::atomic<int64_t> retry_after_{0};  // 自動精煉失敗後，一小時內不再自動重試
};
