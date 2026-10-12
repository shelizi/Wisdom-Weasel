#pragma once

// 輸入法控制器：Rime 之外的所有輸入法行為（Windows 與 mac 共用）
//   - 中英混打（Shift）、注音逐字選字（←/→）、組字中打標點、注音的 Backspace
//   - LLM 預測：送出後預測下一個詞、打字停頓時補全、整句校正、推薦；` 鍵手動觸發、雙擊清空前文
//   - 送出時記錄前文、個人詞庫與選字統計
//   - 注音預覽的組字區（preedit_type: preview）
// 平台相關的部分（候選窗、程式間通訊、前景視窗、重新部署）由 Frontend 提供。
// 除了背景執行緒的回呼，所有方法都在平台的服務端鎖（Frontend::ApiMutex）下呼叫。
#include <rime_api.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "choice_log.h"
#include "session_state.h"

class ContextHistory;
class LearnFilter;
class LLMProvider;
class PersonalLexicon;
class PersonalRefiner;
struct ZhuyinSpeller;  // ZhuyinPreview.h，全域型別

namespace ime {

struct ChoiceStats;
class ChoiceStatsStore;
class CalibrationStore;
class SentenceFinder;
struct RerankResult;
class RankerModel;
class ScriptConverter;
enum class CandidateKind : uint8_t;
class HomophoneFinder;
class PredictionEngine;
struct PredictionSet;

class Frontend {
 public:
  virtual ~Frontend() = default;
  // session 的狀態；已關掉的 session 回傳 nullptr（不可建立新的）
  virtual SessionState* Session(uint64_t id) = 0;
  // 依目前的狀態更新候選窗（呼叫端已持有 ApiMutex）
  virtual void Refresh(uint64_t id) = 0;
  // 收起候選窗
  virtual void HideCandidates() = 0;
  // 服務端的鎖：背景執行緒碰 Rime 或介面前要先拿
  virtual std::mutex& ApiMutex() = 0;
  // 目前輸入的地方（應用程式＋視窗），各自一份前文
  virtual std::wstring ContextKey(uint64_t id) = 0;
  // 請輸入法重新部署（個人詞表有變動時）
  virtual void Redeploy() = 0;
  // 關掉所有 session 放開使用者詞典（整理選字記憶時；呼叫端已持有 ApiMutex）。停用中回傳 false
  virtual bool ReleaseSessions() = 0;
  // 短暫顯示一段提示（例如切換繁簡輸出；呼叫端已持有 ApiMutex）
  virtual void ShowTip(uint64_t id, const std::wstring& text) {}
};

// 按鍵的結果
struct KeyResult {
  bool handled = false;  // 按鍵被輸入法吃掉
  bool respond = true;   // 要回應目前的狀態（送出的文字、組字區、候選）並更新候選窗
};

// 組字區：選取範圍與游標是 wchar_t 的位置；cursor < 0 表示沒有游標資訊
struct Preedit {
  std::wstring text;
  int sel_start = 0;
  int sel_end = 0;
  int cursor = -1;
};

class Controller {
 public:
  struct Options {
    std::filesystem::path user_dir;    // Rime 使用者資料夾
    std::filesystem::path shared_dir;  // Rime 共用資料夾
    std::string config_id = "weasel";  // 前端的設定檔：weasel（小狼毫）／squirrel（鼠鬚管）
    // 選字統計的「組合」用：版本、編譯時間、版本說明
    std::string version;
    std::string build_time;
    std::string build_subject;
    // 簡體在地化還沒改好時，送出能不能等一下（llm/simplified/wait_ms）。
    // 小狼毫可以：放開服務端的鎖，只有那個應用程式在等。鼠鬚管不行：所有輸入都在主執行緒，
    // 等的時候每個應用程式都打不了字，所以只用停頓時已經改好的句子
    bool wait_on_commit = true;
  };

  Controller(RimeApi* api, Frontend* frontend, Options options);
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  void SetContextHistory(ContextHistory* history) { history_ = history; }
  ContextHistory* History() const { return history_; }

  // 讀前端設定檔（Options::config_id）的 llm/*，載入模型與個人詞庫（重新部署時也呼叫）。
  // 模型在背景載入：回傳時還沒載好，載好之前預測、校正與推薦不出現
  void LoadConfig(RimeConfig* config);
  // 停用或重新部署前：停止精煉並存檔（在背景做，不在這裡等）
  void Retire();
  // 直接指定預測用的模型（測試或自訂前端用；LoadConfig 會換掉）
  void SetPredictionModel(std::unique_ptr<LLMProvider> model, bool enabled = true);
  // 在開發終端顯示模型狀態
  void LogStatus() const;

  // 按鍵：keycode 是 X11 keysym，mask 是 Rime 的修飾鍵（keys.h）
  KeyResult ProcessKey(uint64_t id, int keycode, int mask);
  // 候選窗點選第 index 個候選（Rime 候選之後接著 LLM 候選）；送出的是 LLM 候選時回傳 true
  bool SelectCandidate(uint64_t id, size_t index);
  // 應用程式要求送出、清除組字
  void CommitComposition(uint64_t id);
  void ClearComposition(uint64_t id);
  void FocusOut(uint64_t id);

  // 回應前：依序取出要送給應用程式的文字，同時記錄前文、個人詞庫與選字統計
  std::vector<std::wstring> TakeCommits(uint64_t id);
  // 回應前：更新組字狀態（沒在組字時清掉預覽快取與這次組字的統計；組字中記錄換字與預設轉換）。
  // ctx 為 nullptr 表示取不到 Rime 的內容
  void UpdateComposition(uint64_t id, bool composing, const RimeContext* ctx);
  // 注音預覽的組字區；ascii_mode（西文）下或 Rime 沒有預覽時回傳 false
  bool PreviewPreedit(uint64_t id, const RimeContext& ctx, bool ascii_mode, Preedit* out);
  // 混打：把混打內容接在 Rime 的組字區前面（rime 為 nullptr 表示 Rime 沒有組字）
  static Preedit WithMixedText(const SessionState& ss, const Preedit* rime);

  // 候選窗在整句後面顯示的機率（llm/choice/show_probability）：本機模型評過的 Rime 整句
  // 彼此比較的機率。沒開、還沒評完或不是評過的句子回傳 false
  bool SentenceProbability(const std::wstring& text, double* probability) const;
  bool HasSentenceProbabilities() const { return show_probability_ && !sentence_probs_.empty(); }
  // sentence_probs_ 的內容版本：候選資訊快取用它判斷機率有沒有更新
  uint64_t SentenceProbsVersion() const { return probs_gen_; }

  // LLM 候選（接在 Rime 候選後面顯示）
  bool PredictionMode() const { return prediction_mode_; }
  bool ShowingPredictions() const;  // 預測模式中且有候選
  PredictionSet Predictions() const;

  // 簡體輸出（llm/simplified/*）：組字與候選照常是繁體，送出的文字轉成簡體。
  // 設定決定重新部署後的預設；快速鍵與系統匣選單隨時切換（到下次重新部署）。任何執行緒都可以呼叫
  bool SimplifiedOutput() const { return simplified_; }
  bool SetSimplifiedOutput(bool on);  // 轉換字典載入失敗時開不了，回傳 false
  // 測試或自訂前端用：直接指定轉換字典與在地化模型（wait_ms：送出時最多等在地化多久）；LoadConfig 會換掉
  bool SetSimplifiedConversion(const std::filesystem::path& opencc_config,
                               std::unique_ptr<LLMProvider> localize, unsigned wait_ms);

  // 設定程式的指令：1 更新狀態 2 精煉 3 重新精煉全部 4 清除 5 匯出詞彙
  // 6 套用修改並匯出 7 產生 Rime 詞典 8 重設選字統計；狀態寫在 personal/status.txt。
  // 9 暫停模型（設定程式的模型測試：放掉所有模型、精煉不載入模型，放完寫 llm_paused.txt）
  // 10 恢復（照最近一次的設定重新載入）；重新部署也會恢復
  void PersonalCommand(int command);
  // 設定程式的「測試模型」：讀 llm_test_request.txt，在背景預測並寫 llm_test_response.txt
  void LLMTest();

 private:
  struct ModelPlan;

  bool TypoAvailable() const;
  bool PredictionAvailable() const;  // LLM 或個人詞庫至少一個可用（且總開關開啟）
  LLMProvider* RescoreProvider() const;
  // 讀校正、評分要用的模型（在 LoadConfig 裡）；載入交給背景的 LoadModels
  void PlanTypo(RimeConfig* config, ModelPlan* plan);
  void PlanScorer(RimeConfig* config, ModelPlan* plan);
  void PlanLocalize(RimeConfig* config, ModelPlan* plan);
  // 簡體在地化：排進背景改寫（urgent：送出時要，不等停頓）；送出時取結果（等不到回傳 false）
  void RequestLocalize(const std::wstring& text, bool urgent);
  bool WaitLocalized(const std::wstring& text, std::wstring* result);
  void LocalizeLoop();
  std::wstring ComposingSentence(RimeSessionId session_id);
  // 背景：照 LoadConfig 讀好的設定啟動推理行程、載入模型，載好再換上
  void ModelLoaderLoop();
  void LoadModels(ModelPlan& plan);
  bool PlanStale(const ModelPlan& plan);
  // 交給模型載入執行緒：先釋放 retired（換下來的舊模型），再照 plan 載入
  void PostModels(std::unique_ptr<ModelPlan> plan, std::vector<std::unique_ptr<LLMProvider>> retired);
  // 放下目前的模型（在推理鎖下），回傳要釋放的
  std::vector<std::unique_ptr<LLMProvider>> TakeModels();
  void PauseModels();
  void ResumeModels();
  void UpdateContextKey(uint64_t id);

  bool HandleSimplifiedKey(uint64_t id, int keycode, int mask);
  bool HandleMixedInput(uint64_t id, SessionState& ss, int keycode, int mask, bool* respond);
  bool HandleZhuyinFocus(uint64_t id, SessionState& ss, int keycode, int mask);
  bool HandleZhuyinDelete(SessionState& ss, int keycode, int mask);
  bool FocusSyllable(SessionState& ss, int index);
  void LockFocusedChoice(SessionState& ss, int index, const std::wstring& text);
  size_t ConfirmedPrefix(SessionState& ss);
  bool CaptureManualChoice(SessionState& ss, int index, const std::string& input, bool focused);
  void SyncManualChoices(SessionState& ss, const std::string& input);
  void EditManualChoices(SessionState& ss, size_t start, size_t erased, size_t inserted);
  bool RestoreManualChoices(SessionState& ss, bool keep_focus = true);

  void TriggerPrediction(uint64_t id, const std::wstring& current_input = L"", unsigned delay_ms = 0,
                         const std::wstring& completion_prefix = L"", const std::wstring& zhuyin = L"",
                         bool complete = true);
  void ScheduleCompletion(uint64_t id, unsigned delay_ms, const char* schema_id = nullptr);
  void CancelCompletion();
  void ExitPredictionMode(uint64_t id);
  bool CommitPrediction(uint64_t id, size_t index);
  bool RescoreInput(uint64_t id, uint64_t seq, std::vector<std::wstring>* units,
                    std::vector<std::vector<std::wstring>>* homophones);
  // 整句重排：Rime 對目前輸入的整句候選（第一句是使用者看到的轉換）
  bool RerankInput(uint64_t id, uint64_t seq, std::vector<std::wstring>* sentences);
  void OnShadow(uint64_t id, uint64_t seq, const std::wstring& first, const RerankResult& result);
  void OnSentenceScores(uint64_t id, uint64_t seq, const RerankResult& result);
  // 送出時對照 shadow 的結果（選字統計與校準）
  void CountShadow(SessionState& ss, const std::wstring& text);
  // 推薦或整句重排要用本機模型評分
  bool ScoringWanted() const;
  void OnPredictionUpdate(uint64_t id, uint64_t seq, const PredictionSet& set);
  // 信心校準：把顯示中的推薦／校正記成樣本（taken 是選了哪一種，都沒選是 kPrediction）
  void RecordSuggestions(SessionState& ss, CandidateKind taken);
  static void ForgetSuggestions(SessionState& ss);
  bool ConfirmText(SessionState& ss, const std::wstring& desired);
  std::wstring ComposingZhuyin(RimeSessionId session_id, const char* schema_id);

  std::string ChoiceProfile(RimeSessionId session_id);
  ChoiceStats& Stats(RimeSessionId session_id);
  void SaveStats();
  // 目前方案的拼寫設定（注音預覽、組字區標點用）。方案檔案在執行中不會變，
  // LoadConfig（重新部署）時清掉
  const ZhuyinSpeller& Speller(const char* schema_id);
  void CountCommit(SessionState& ss, const std::wstring& text, bool mixed = false);
  void LogChoice(SessionState& ss, const std::wstring& text, bool mixed,
                 const std::string& rec);
  void RecordCommit(const std::wstring& text);  // 前文與個人詞庫
  void WaitRetired();

  RimeApi* api_;
  Frontend* frontend_;
  Options options_;
  ContextHistory* history_ = nullptr;

  // 設定（llm/*）
  bool llm_enabled_ = false;       // llm/enabled：所有預測候選的總開關
  bool after_commit_ = true;       // llm/predict_after_commit：送出後預測下一個詞
  bool while_typing_ = true;       // llm/predict_while_typing：打字停頓時自動補完
  bool typo_on_ = false;           // llm/typo/llm：LLM 整句校正
  bool rescore_on_ = false;        // llm/choice/rescore：推薦
  double min_confidence_ = 0.5;    // llm/choice/min_confidence：推薦／校正的採用機率低於這個就不顯示
  // llm/choice/rerank：整句重排（Rime Top-K 由本機模型重排）。0 關、1 shadow（只算不顯示）、2 顯示
  int rerank_mode_ = 0;
  double rerank_margin_ = 2.0;     // llm/choice/rerank_margin
  bool show_probability_ = false;  // llm/choice/show_probability：候選窗顯示整句機率
  std::map<std::wstring, double> sentence_probs_;  // 目前輸入的整句 → 機率（服務端的鎖下讀寫）
  uint64_t probs_gen_ = 0;  // sentence_probs_ 每次變動加一
  size_t rerank_sentences_ = 10;   // llm/choice/rerank_sentences：最多評幾句
  size_t rerank_context_ = 30;     // llm/choice/rerank_context：給模型的前文最多幾個字
  double rerank_alpha_ = 0.0;      // llm/choice/rerank_alpha：長度正規化（0 = 不正規化）
  double rerank_personal_ = 0.0;   // llm/choice/rerank_personal：個人詞庫加分的權重（0 = 不用）
  bool choice_log_ = false;        // llm/choice/log：記錄選字過程（加密）
  size_t context_max_chars_ = 100;      // llm/context/max_chars：給模型的前文最多幾個字
  unsigned context_idle_minutes_ = 10;  // llm/context/idle_minutes：視窗閒置多久後舊前文失效
  size_t personal_max_ = 3;             // 候選中最多幾個來自個人詞庫
  // 簡體輸出：轉換器（OpenCC）、目前是否轉換、切換的快速鍵（keysym 小寫與修飾鍵；0 = 沒有）
  std::unique_ptr<ScriptConverter> simplifier_;
  std::atomic<bool> simplified_{false};
  int simplified_key_ = 0;
  int simplified_mask_ = 0;
  bool simplified_key_down_ = false;  // 按下時切換了，放開也吃掉
  // 簡體在地化（llm/simplified/config: llm）：打字停頓時先在背景改寫目前的句子，送出時用現成的結果；
  // 還沒好就放開服務端的鎖等一下（llm/simplified/wait_ms），等不到或改寫不能用就用 OpenCC（tw2sp）
  bool localize_on_ = false;
  unsigned localize_wait_ms_ = 1000;
  std::unique_ptr<LLMProvider> localize_owned_;
  LLMProvider* localize_llm_ = nullptr;  // 可能指向 llm_provider_ 或 typo_llm_（推理鎖下換）
  std::thread localize_thread_;
  std::mutex localize_mutex_;
  std::condition_variable localize_cv_;
  std::wstring localize_pending_;   // 等著改寫的（只留最新的）
  bool localize_urgent_ = false;    // 送出時要：不等停頓
  std::wstring localize_running_;   // 正在改寫的
  std::map<std::wstring, std::wstring> localized_;  // 改好的：原句 → 結果（空字串 = 不能用）
  std::vector<std::wstring> localized_order_;       // 最多留 64 句（先進先出）
  bool localize_stop_ = false;
  std::mutex typo_prompt_mutex_;
  std::wstring typo_prompt_;  // llm/typo/prompt：自訂校正指令

  // 模型：在推理鎖（prediction_->InferMutex()）下更換與取用
  std::unique_ptr<LLMProvider> llm_provider_;
  std::unique_ptr<LLMProvider> typo_owned_;
  // llm/choice/scorer/*：評分用的本機模型（推薦、整句重排、學習過濾）；和預測或校正選同一個
  // 模型檔時共用（scorer_llm_ 指過去），都借不到才自己載入
  std::unique_ptr<LLMProvider> scorer_owned_;
  LLMProvider* scorer_llm_ = nullptr;  // 可能指向 llm_provider_ 或 typo_llm_
  LLMProvider* typo_llm_ = nullptr;    // 可能指向 llm_provider_
  std::wstring loaded_model_;        // 目前載入的模型（設定畫面顯示用）
  // 模型載入執行緒：啟動推理行程、載入模型要好幾秒，不能在服務端的鎖下做（所有應用程式都在等）。
  // LoadConfig 放一份計畫進來（只留最新的），載好後再拿鎖換上；設定又變了就丟掉舊的結果
  std::thread loader_thread_;
  std::mutex loader_mutex_;
  std::condition_variable loader_cv_;
  std::unique_ptr<ModelPlan> loader_plan_;
  std::vector<std::unique_ptr<LLMProvider>> loader_retired_;  // 等著釋放的舊模型
  bool loader_stop_ = false;
  std::unique_ptr<ModelPlan> last_plan_;  // 最近一次設定要的模型（恢復時照著載入）
  std::atomic<bool> models_paused_{false};  // 設定程式的模型測試中（精煉的執行緒也會讀）
  std::atomic<uint64_t> model_generation_{0};  // LoadConfig 每次加一

  std::unique_ptr<PersonalLexicon> personal_;
  std::unique_ptr<PersonalRefiner> refiner_;
  // 學習過濾（llm/personal/filter/*）：送出的文字先用本機模型評分再學；要比 personal_ 先釋放
  std::unique_ptr<LearnFilter> learn_filter_;
  // 停用或重新部署時，精煉器與個人詞庫交給這條執行緒停止、存檔再釋放
  std::thread retire_thread_;

  // 學習式整句排序器（personal/ranker_model.json；請求帶 shared_ptr 給引擎）
  std::shared_ptr<RankerModel> ranker_model_;
  std::filesystem::file_time_type ranker_model_mtime_{};
  int64_t ranker_model_check_ms_ = 0;

  std::unique_ptr<PredictionEngine> prediction_;
  std::unique_ptr<HomophoneFinder> homophones_;
  std::unique_ptr<SentenceFinder> sentences_;
  std::unique_ptr<ChoiceStatsStore> choice_store_;
  // Speller() 的快取：按鍵路徑（組字中打標點）與每次組字區更新都會用
  std::string speller_schema_;
  std::unique_ptr<ZhuyinSpeller> speller_;
  std::unique_ptr<CalibrationStore> calibration_;  // 推薦／校正的信心校準（預測背景執行緒也會讀）
  std::map<std::string, std::string> schema_flags_;  // 方案 → 方案裡的設定（語言模型等）

  // 預測狀態
  bool prediction_mode_ = false;    // 顯示 LLM 候選中
  bool completion_active_ = false;  // 目前的 LLM 候選是打字中的補全（Rime 首選 + 續寫）
  std::wstring pending_commit_;     // 待送出的 LLM 候選
  bool mixed_shift_tap_ = false;    // Shift 按下後還沒按其他鍵（放開時算一次切換）
  uint64_t last_commit_ms_ = 0;     // 最近一次送出的時間（算送出後刪除）
  // 最近一筆選字紀錄：送出後很快在應用程式裡刪字時補記 method=reverted
  ChoiceRecord last_choice_;
  bool last_choice_reverted_ = false;
  uint64_t last_grave_ms_ = 0;      // 上次按 ` 的時間（雙擊清空前文）
  // 精煉借用輸入法的模型時：打字或要換模型就讓出來（背景執行緒讀）
  std::atomic<uint64_t> last_key_ms_{0};   // 最近一次按鍵的時間
  std::atomic<uint64_t> model_wanted_{0};  // 打字、換模型時加一：進行中的借用對話看到就中斷
};

}  // namespace ime
