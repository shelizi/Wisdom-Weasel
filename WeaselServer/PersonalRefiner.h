#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include <vector>
#include "PersonalLexicon.h"

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
    bool UsesLLM() const {
      return (type == "openai" && !api_url.empty()) || (type == "llamacpp" && !model_path.empty());
    }
  };

  explicit PersonalRefiner(PersonalLexicon* lexicon);
  ~PersonalRefiner();

  void Configure(const Config& config);
  void Start();  // 啟動排程執行緒
  void Stop();   // 停止並等待進行中的精煉結束

  // 在背景執行精煉；已有一個在跑時回傳 false
  bool RunAsync(bool full);
  bool Running() const { return running_; }

  // 清除所有資料（精煉中時不做事，回傳 false）
  bool Clear();

  // 重新寫 status.txt
  void WriteStatus() { WriteStatusAs(running_); }

 private:
  void SchedulerLoop();
  void Run(bool full);
  // 呼叫 LLM 精煉（分批審查）；records 用來挑例句。回傳結果說明文字，失敗時 ok = false
  //   一般精煉：只審查還沒審查過的詞（最近出現的優先），一批
  //   重新精煉全部：所有的詞分批審查完
  std::wstring RefineWithLLM(const Config& config, bool full,
                             const std::vector<PersonalLexicon::RawRecord>& records,
                             bool* ok);

  std::wstring CallRemote(const Config& config, const std::wstring& user, size_t words,
                          size_t examples, bool* ok);
  // 解析並套用一批的結果；回傳這批刪除的詞，merged 累加合併數
  std::vector<std::wstring> ApplyLLMResult(const std::wstring& content, size_t batch_size,
                                           const std::unordered_set<std::wstring>& known,
                                           size_t* merged);
  std::string Method() const;  // 精煉方式（status.txt 顯示用，呼叫時已持有 mutex_）
  void WriteStatusAs(bool running);

  PersonalLexicon* lexicon_;
  mutable std::mutex mutex_;  // config_、last_result_、排程等待
  std::mutex run_mutex_;      // worker_ 的啟動與結束
  Config config_;
  std::wstring last_result_;
  std::wstring progress_;  // 分批進度（例如「第 3／17 批」），status.txt 顯示用
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::condition_variable cv_;
  std::thread scheduler_;
  std::thread worker_;
  std::atomic<int64_t> retry_after_{0};  // 自動精煉失敗後，一小時內不再自動重試
};
