#include "controller.h"

#include <logging.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

#include "../base/clock.h"
#include "../base/devlog.h"
#include "../base/utf8.h"
#include "../llm/ContextHistory.h"
#include "../llm/LLMProvider.h"
#include "../llm/RemoteLLMProvider.h"
#include "../personal/PersonalLexicon.h"
#include "../personal/PersonalRefiner.h"
#include "ZhuyinPreview.h"
#include "choice_log.h"
#include "choice_stats.h"
#include "keys.h"
#include "prediction_engine.h"
#include "rime_helpers.h"
#include "text_rules.h"

namespace ime {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kGraveDoubleClickMs = 500;  // 雙擊 ` 的時間間隔
constexpr unsigned kCompletionDelayMs = 300;   // 打字中停頓多久觸發補全預測

bool Logging() {
  return g_dev_console && g_dev_console->IsEnabled();
}

void Log(const std::wstring& text) {
  if (Logging())
    g_dev_console->WriteLine(text);
}

bool IsZhuyinSchema(const char* schema_id) {
  return schema_id && std::strncmp(schema_id, "bopomofo", 8) == 0;
}

// 去掉 UTF-16 代理對的前半（刪掉最後一個字時）
void PopChar(std::wstring& s) {
  if (s.empty())
    return;
  s.pop_back();
  if (sizeof(wchar_t) == 2 && !s.empty() && s.back() >= 0xD800 && s.back() <= 0xDBFF)
    s.pop_back();
}

}  // namespace

Controller::Controller(RimeApi* api, Frontend* frontend, Options options)
    : api_(api), frontend_(frontend), options_(std::move(options)) {
  homophones_ = std::make_unique<HomophoneFinder>(api_);
  choice_store_ = std::make_unique<ChoiceStatsStore>(options_.user_dir);
  PredictionEngine::Hooks hooks;
  hooks.models = [this] {
    PredictionModels models;
    models.predict = llm_provider_ && llm_provider_->IsAvailable() ? llm_provider_.get() : nullptr;
    models.typo = typo_llm_;
    models.rescore = RescoreProvider();
    return models;
  };
  hooks.rescore_input = [this](uint64_t tag, uint64_t seq, std::vector<std::wstring>* units,
                               std::vector<std::vector<std::wstring>>* homophones) {
    return RescoreInput(tag, seq, units, homophones);
  };
  hooks.on_update = [this](uint64_t tag, uint64_t seq, const PredictionSet& set) {
    OnPredictionUpdate(tag, seq, set);
  };
  prediction_ = std::make_unique<PredictionEngine>(std::move(hooks));
}

Controller::~Controller() {
  prediction_->Cancel();
  WaitRetired();
}

// ---------------------------------------------------------------------------
// 設定與模型

void Controller::LoadConfig(RimeConfig* config) {
  // 先釋放舊模型（重新部署時），避免新舊模型同時佔用記憶體，或關閉 LLM 後舊模型仍駐留。
  // 作廢排隊中的預測，並等進行中的推理結束，避免背景執行緒用到已釋放的模型
  prediction_->Cancel();
  std::lock_guard<std::mutex> infer_lock(prediction_->InferMutex());
  typo_llm_ = nullptr;  // 可能指向 llm_provider_，先放掉
  typo_owned_.reset();
  llm_provider_.reset();
  loaded_model_.clear();
  // 兩種自動觸發時機可分別關閉（未設定時預設開啟）；關閉後仍可按 ` 鍵手動觸發
  Bool flag = true;
  after_commit_ = !api_->config_get_bool(config, "llm/predict_after_commit", &flag) || flag;
  flag = true;
  while_typing_ = !api_->config_get_bool(config, "llm/predict_while_typing", &flag) || flag;
  // 選字紀錄（加密、只在本機，預設關閉）
  flag = false;
  choice_log_ = api_->config_get_bool(config, "llm/choice/log", &flag) && flag;
  // 推薦：本機模型比較同音字整句的通順度（預設關閉）
  flag = false;
  rescore_on_ = api_->config_get_bool(config, "llm/choice/rescore", &flag) && flag;
  homophones_->ClearCache();  // 重新部署後詞典可能變了
  // LLM 整句校正（llm/typo/llm）；Rime 容錯另外設定在注音方案裡，與此無關。
  // 舊設定 llm/typo_correction: llm 視為開啟
  {
    Bool typo = false;
    char legacy[32] = {0};
    if (api_->config_get_bool(config, "llm/typo/llm", &typo))
      typo_on_ = !!typo;
    else
      typo_on_ = api_->config_get_string(config, "llm/typo_correction", legacy, sizeof(legacy) - 1) &&
                 std::strcmp(legacy, "llm") == 0;
  }
  // 前文：每個視窗各自一份；只給模型最後 max_chars 個字；視窗閒置 idle_minutes 後舊前文失效
  int value = 0;
  context_max_chars_ =
      api_->config_get_int(config, "llm/context/max_chars", &value) && value > 0 ? (size_t)value : 100;
  // 個人詞庫：從送出的文字學習常用詞與接續（預設開啟；資料加密存在使用者資料夾）
  {
    Bool personal_enabled = true;
    if (!api_->config_get_bool(config, "llm/personal/enabled", &personal_enabled))
      personal_enabled = true;
    int n = 0;
    personal_max_ = api_->config_get_int(config, "llm/personal/max_candidates", &n) && n >= 0
                        ? (size_t)(std::min)(n, 5)
                        : 3;
    if (personal_enabled && !personal_) {
      WaitRetired();  // 上一份還在存檔時先等它，避免同時讀寫檔案（已要求停止，通常很快）
      personal_ = std::make_unique<PersonalLexicon>(options_.user_dir / "personal");
      personal_->Load();
    } else if (!personal_enabled && personal_) {
      Retire();  // 背景停止精煉並存檔，不在這裡等
    }
    if (personal_) {
      n = 0;
      if (api_->config_get_int(config, "llm/personal/half_life_days", &n) && n > 0)
        personal_->SetHalfLifeDays(n);
      Bool keep_log = true;
      if (!api_->config_get_bool(config, "llm/personal/keep_raw_log", &keep_log))
        keep_log = true;
      personal_->SetKeepRawLog(!!keep_log);

      // 定時精煉：預設每 1 天（0 = 只手動）。精煉用的模型由設定程式從「語言模型」清單選用後
      // 寫到 llm/personal/refine/*（type 為 llamacpp 或 openai；沒選時只做統計整理）
      PersonalRefiner::Config refine;
      char buf[1024] = {0};
      if (api_->config_get_string(config, "llm/personal/refine/interval_days", buf, sizeof(buf)))
        refine.interval_days = (std::max)(0.0, std::atof(buf));
      auto read_string = [&](const char* key) {
        char text[2048] = {0};
        return api_->config_get_string(config, key, text, sizeof(text)) ? std::string(text)
                                                                          : std::string();
      };
      refine.type = read_string("llm/personal/refine/type");
      refine.name = read_string("llm/personal/refine/name");
      refine.api_url = read_string("llm/personal/refine/api_url");
      refine.api_key = read_string("llm/personal/refine/api_key");
      refine.model = read_string("llm/personal/refine/model");
      refine.model_path = read_string("llm/personal/refine/model_path");
      std::string refine_model_type = read_string("llm/personal/refine/model_type");
      std::transform(refine_model_type.begin(), refine_model_type.end(), refine_model_type.begin(),
                     ::tolower);
      refine.instruct = refine_model_type != "base";
      refine.disable_thinking = read_string("llm/personal/refine/disable_thinking") == "true";
      {
        int think_tokens = 2048;
        if (api_->config_get_int(config, "llm/personal/refine/think_tokens", &think_tokens))
          refine.think_tokens = (std::max)(0, think_tokens);
      }
      // 舊設定（只有 api_url）視為 OpenAI 相容 API
      if (refine.type.empty() && !refine.api_url.empty() &&
          read_string("llm/personal/refine/profile").empty())
        refine.type = "openai";
      // 讓常打的詞影響注音選字排序（設定程式負責修改方案；這裡負責產生詞典）
      Bool rime_boost = false;
      refine.rime_boost =
          api_->config_get_bool(config, "llm/personal/rime_boost", &rime_boost) && rime_boost;
      refine.rime_dict_path = (options_.user_dir / "terra_pinyin.personal.dict.yaml").wstring();
      refine.essay_path = (options_.shared_dir / "essay.txt").wstring();
      refine.redeploy = [frontend = frontend_] { frontend->Redeploy(); };
      // 本機精煉沿用預測的 GPU / 執行緒設定
      int llama = 0;
      if (api_->config_get_int(config, "llm/llamacpp/n_gpu_layers", &llama))
        refine.n_gpu_layers = llama;
      if (api_->config_get_int(config, "llm/llamacpp/n_threads", &llama) && llama > 0)
        refine.n_threads = llama;
      if (!refiner_) {
        refiner_ = std::make_unique<PersonalRefiner>(personal_.get());
        // 整理選字記憶時要匯出／匯入使用者詞典：在服務端的鎖下關掉所有 session 放開詞典，
        // 客戶端之後打字時發現 session 不在會自動重建
        refiner_->SetUserDictAccess([frontend = frontend_](const std::function<void()>& fn) {
          // 不能一直等鎖：重新載入設定時服務端會持鎖等精煉結束，一直等就互相卡死
          std::unique_lock<std::mutex> lock(frontend->ApiMutex(), std::defer_lock);
          for (int i = 0; i < 100 && !lock.try_lock(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          if (!lock.owns_lock() || !frontend->ReleaseSessions())
            return false;
          fn();
          return true;
        });
        refiner_->Configure(refine);
        refiner_->Start();
      } else {
        refiner_->Configure(refine);
        refiner_->WriteStatus();
      }
    }
  }
  value = 0;
  context_idle_minutes_ =
      api_->config_get_int(config, "llm/context/idle_minutes", &value) && value >= 0 ? (unsigned)value
                                                                                     : 10;
  Bool enabled = false;
  // 「啟用 LLM 智慧預測」是所有預測候選的總開關：關閉時個人詞庫只在背景學習，不跳出候選
  llm_enabled_ = api_->config_get_bool(config, "llm/enabled", &enabled) && enabled;
  if (llm_enabled_) {
    char type_buf[65] = {0};
    std::string provider_type = "openai";  // 預設值
    if (api_->config_get_string(config, "llm/provider_type", type_buf, 64))
      provider_type = type_buf;
    // 依 provider_type 建立 provider；模型在獨立的推理行程裡執行，當掉不影響打字
    if (provider_type == "llamacpp" || provider_type == "hf_constraint")
      llm_provider_ = std::make_unique<RemoteLLMProvider>(provider_type);
    else
      llm_provider_ = std::make_unique<RemoteLLMProvider>("openai");  // 預設用 OpenAI 相容 API
    LOG(INFO) << "LLM Provider type: " << provider_type;
    if (llm_provider_->LoadConfig("weasel")) {
      LOG(INFO) << "LLM Provider initialized successfully: " << llm_provider_->GetProviderName();
      // 記下目前載入的模型，供設定畫面顯示
      char model_buf[1024] = {0};
      char url_buf[1024] = {0};
      if (provider_type == "llamacpp" &&
          api_->config_get_string(config, "llm/llamacpp/model_path", model_buf, sizeof(model_buf) - 1)) {
        loaded_model_ = utf8::ToWide(model_buf);
      } else if (provider_type == "openai") {
        // 顯示為「OpenAI 相容 API：模型（網址）」
        api_->config_get_string(config, "llm/openai/model", model_buf, sizeof(model_buf) - 1);
        api_->config_get_string(config, "llm/openai/api_url", url_buf, sizeof(url_buf) - 1);
        loaded_model_ = L"OpenAI 相容 API：" + utf8::ToWide(model_buf) + L"（" + utf8::ToWide(url_buf) + L"）";
      } else {
        loaded_model_ = utf8::ToWide(llm_provider_->GetProviderName());
      }
    } else {
      LOG(ERROR) << "LLM Provider initialization failed (llm/provider_type: " << provider_type
                 << "); check llm/* in weasel.yaml";
      llm_provider_.reset();
    }
  } else {
    LOG(INFO) << "LLM is disabled or not configured (llm/enabled)";
  }
  // 注音整句校正的模型：不受「智慧預測」開關影響
  LoadTypoProvider(config);
}

void Controller::LoadTypoProvider(RimeConfig* config) {
  typo_llm_ = nullptr;
  typo_owned_.reset();
  if (!typo_on_)
    return;
  auto read = [&](const char* key) {
    char value[4096] = {0};
    return api_->config_get_string(config, key, value, sizeof(value) - 1) ? std::string(value)
                                                                          : std::string();
  };
  // 校正使用的模型由設定程式從「語言模型」清單選用後展開到 llm/typo/*
  const std::string type = read("llm/typo/type");
  const std::string model_path = read("llm/typo/model_path");
  std::string model_type = read("llm/typo/model_type");
  std::transform(model_type.begin(), model_type.end(), model_type.begin(), ::tolower);
  const std::string api_url = read("llm/typo/api_url");
  const std::string api_key = read("llm/typo/api_key");
  const std::string model = read("llm/typo/model");
  std::string prompt = read("llm/prompt");
  if (prompt.empty())
    prompt = read("llm/llamacpp/prompt_prefix");
  {
    std::lock_guard<std::mutex> lock(typo_prompt_mutex_);
    typo_prompt_ = utf8::ToWide(read("llm/typo/prompt"));  // 自訂校正指令，空字串用預設
  }
  const bool no_think = read("llm/typo/disable_thinking") == "true";
  int think_tokens = 2048;  // 開啟思考時的思考長度上限（0 = 不限制）
  if (api_->config_get_int(config, "llm/typo/think_tokens", &think_tokens))
    think_tokens = (std::max)(0, think_tokens);
  else
    think_tokens = 2048;

  // 和智慧預測是同一個模型：共用，避免同一個模型載入兩次
  if (llm_provider_ && llm_provider_->IsAvailable()) {
    const std::string provider_type = read("llm/provider_type");
    std::string predict_type = read("llm/llamacpp/model_type");
    std::transform(predict_type.begin(), predict_type.end(), predict_type.begin(), ::tolower);
    const bool same_local = type == "llamacpp" && provider_type == "llamacpp" &&
                            model_path == read("llm/llamacpp/model_path") &&
                            (model_type == "base") == (predict_type == "base");
    const bool same_api = type == "openai" && provider_type == "openai" &&
                          api_url == read("llm/openai/api_url") &&
                          api_key == read("llm/openai/api_key") && model == read("llm/openai/model");
    if (same_local || same_api) {
      typo_llm_ = llm_provider_.get();
      LOG(INFO) << "Typo correction shares the prediction model.";
      return;
    }
  }
  if (type == "llamacpp" && !model_path.empty()) {
    LLMLocalModelSpec spec;
    spec.model_path = model_path;
    spec.instruct = model_type != "base";
    // 前文 + 一句話 2048 就夠；開啟思考時再加上思考額度（不限制時給 8192）
    spec.n_ctx = no_think || model_type == "base" ? 2048
                 : think_tokens <= 0              ? 8192
                                                  : (std::min)(2048 + think_tokens, 32768);
    spec.disable_thinking = no_think;
    spec.think_tokens = think_tokens;
    int value = 0;
    if (api_->config_get_int(config, "llm/llamacpp/n_gpu_layers", &value))
      spec.n_gpu_layers = value;
    if (api_->config_get_int(config, "llm/llamacpp/n_threads", &value) && value > 0)
      spec.n_threads = value;
    auto provider = std::make_unique<RemoteLLMProvider>("llamacpp");
    if (provider->LoadModelDirect(spec, 0.0)) {
      provider->SetPromptPrefix(utf8::ToWide(prompt));
      typo_owned_ = std::move(provider);
    } else {
      LOG(ERROR) << "Typo correction: failed to load model " << model_path;
    }
  } else if (type == "openai" && !api_url.empty()) {
    auto provider = std::make_unique<RemoteLLMProvider>("openai");
    provider->ConfigureDirect(api_url, api_key, model, utf8::ToWide(prompt), no_think, think_tokens);
    typo_owned_ = std::move(provider);
  } else {
    LOG(WARNING) << "Typo correction: no model selected (llm/typo/type)";
  }
  typo_llm_ = typo_owned_.get();
}

void Controller::SetPredictionModel(std::unique_ptr<LLMProvider> model, bool enabled) {
  prediction_->Cancel();
  std::lock_guard<std::mutex> infer_lock(prediction_->InferMutex());
  if (typo_llm_ == llm_provider_.get())
    typo_llm_ = nullptr;
  llm_provider_ = std::move(model);
  llm_enabled_ = enabled;
}

void Controller::LogStatus() const {
  if (!Logging())
    return;
  if (!llm_provider_) {
    Log(L"[LLM] LLM提供者未初始化");
    Log(L"[LLM] 请在weasel.yaml中配置：llm/enabled: true 与 llm/openai/api_key");
  } else if (!llm_provider_->IsAvailable()) {
    Log(L"[LLM] LLM提供者已初始化，但不可用");
    Log(L"[LLM] 请检查配置：llm/enabled 和 llm/openai/api_key");
  } else {
    Log(L"[LLM] LLM提供者已就绪: " + utf8::ToWide(llm_provider_->GetProviderName()));
  }
}

void Controller::Retire() {
  if (!refiner_ && !personal_)
    return;
  if (refiner_)
    refiner_->RequestStop();  // 串流請求與本機生成會盡快中斷
  WaitRetired();
  retire_thread_ = std::thread(
      [refiner = std::move(refiner_), personal = std::move(personal_)]() mutable {
        refiner.reset();   // 等精煉結束
        personal.reset();  // 解構時存檔
      });
}

void Controller::WaitRetired() {
  if (retire_thread_.joinable())
    retire_thread_.join();
}

bool Controller::TypoAvailable() const {
  return typo_on_ && typo_llm_ && typo_llm_->IsAvailable();
}

bool Controller::PredictionAvailable() const {
  if (!llm_enabled_)
    return false;
  return (llm_provider_ && llm_provider_->IsAvailable()) || personal_ != nullptr;
}

LLMProvider* Controller::RescoreProvider() const {
  auto local = [](LLMProvider* p) {
    return p && p->IsAvailable() && p->GetProviderName() == "llama.cpp Local";
  };
  if (local(llm_provider_.get()))
    return llm_provider_.get();
  if (typo_on_ && local(typo_llm_))
    return typo_llm_;
  return nullptr;
}

void Controller::UpdateContextKey(uint64_t id) {
  if (!history_)
    return;
  history_->SetIdleTimeout(context_idle_minutes_ * 60ull * 1000ull);
  history_->SetActiveKey(frontend_->ContextKey(id), g_dev_console);
}

// ---------------------------------------------------------------------------
// 按鍵

KeyResult Controller::ProcessKey(uint64_t id, int keycode, int mask) {
  SessionState* state = frontend_->Session(id);
  if (!state)
    return {false, true};
  SessionState& ss = *state;
  const RimeSessionId session_id = ss.session_id;
  const bool release = (mask & mod::kRelease) != 0;
  const int other_mod_mask = mod::kControl | mod::kAlt | mod::kSuper;

  // 依目前輸入的視窗切換上下文（之後的送出紀錄與預測都用該視窗自己的前文）
  if (!release)
    UpdateContextKey(id);

  // 中英混打（Shift）
  bool respond = true;
  if (HandleMixedInput(id, ss, keycode, mask, &respond))
    return {true, respond};
  if (HandleZhuyinFocus(id, ss, keycode, mask))
    return {true, true};

  // ` 鍵：組字中觸發 LLM 預測；雙擊清空前文
  if (!release && keycode == key::kGrave) {
    const uint64_t now = base::MonotonicMs();
    const bool double_click = last_grave_ms_ > 0 && now - last_grave_ms_ < kGraveDoubleClickMs;
    last_grave_ms_ = now;
    Log(double_click ? L"[LLM] 检测到双击·键" : L"[LLM] 用户按下·键");
    if (double_click) {
      if (history_) {
        Log(L"[LLM] 双击·键，清空上下文历史记录（清空前记录数: " + std::to_wstring(history_->GetSize()) +
            L"）");
        history_->Clear(g_dev_console);
        Log(L"[LLM] 上下文历史记录已清空");
      } else {
        Log(L"[LLM] 上下文历史记录未初始化，无法清空");
      }
      return {true, false};
    }
    RIME_STRUCT(RimeStatus, status);
    bool composing = false;
    if (api_->get_status(session_id, &status)) {
      composing = !!status.is_composing;
      api_->free_status(&status);
    }
    // 沒在組字時讓 ` 照常輸入
    if (!composing) {
      Log(L"[LLM] 不在composing状态，允许·键正常输入");
    } else if (!llm_provider_) {
      Log(L"[LLM] LLM提供者未初始化，请检查 weasel.yaml 的 llm/enabled 与 llm/openai/api_key");
    } else if (!llm_provider_->IsAvailable()) {
      Log(L"[LLM] LLM提供者已初始化，但不可用（llm/enabled、api_key、api_url）");
    } else {
      Log(L"[LLM] composing状态=true，触发LLM预测");
      if (!prediction_mode_) {
        prediction_mode_ = true;
        Log(history_ && history_->GetSize() > 0 ? L"[LLM] 进入LLM预测模式，将使用上下文历史"
                                                : L"[LLM] 进入LLM预测模式，上下文历史为空");
      }
      // 立即以 Rime 目前的轉換結果做補全預測（注音的 preedit 是注音符號，不適合直接給模型）
      ScheduleCompletion(id, 0);
      return {true, false};
    }
  }

  // 預測模式中的特殊按鍵
  if (prediction_mode_ && !release) {
    // Esc：退出預測模式；正在組字時繼續交給 Rime 清除組字，一次關閉候選欄
    if (keycode == key::kEscape) {
      ExitPredictionMode(id);
      RIME_STRUCT(RimeStatus, status);
      bool composing = false;
      if (api_->get_status(session_id, &status)) {
        composing = !!status.is_composing;
        api_->free_status(&status);
      }
      if (!composing)
        return {true, false};
    }
    // Tab 選第一個 LLM 候選，Shift+1~5 選第幾個
    // （空白與數字鍵在注音大千鍵盤中是注音符號與聲調，不能拿來選）
    int pick = -1;
    if (!(mask & other_mod_mask)) {
      if (keycode == key::kTab && !(mask & mod::kShift)) {
        pick = 0;
      } else if (mask & mod::kShift) {
        static const int kShiftedDigits[] = {'!', '@', '#', '$', '%'};
        for (int k = 0; k < 5; ++k) {
          if (keycode == kShiftedDigits[k] || keycode == '1' + k) {
            pick = k;
            break;
          }
        }
      }
    }
    if (pick >= 0 && CommitPrediction(id, (size_t)pick))
      return {true, true};
    // 打字母：退出預測模式回到一般輸入
    // （打字中補全時字母是注音鍵，由 process_key 之後的補全邏輯接手，不在這裡退出以免候選欄閃爍）
    if (!completion_active_ && ((keycode >= 'a' && keycode <= 'z') || (keycode >= 'A' && keycode <= 'Z')))
      ExitPredictionMode(id);
  }

  // 組字中打標點：Rime 會把組字連同標點直接送出；改為留在組字區（像新注音），等 Enter 一起送出
  bool punct_in_composition = false;
  if (!(mask & (mod::kRelease | other_mod_mask)) && keycode > 0x20 && keycode <= 0x7e &&
      !std::isalnum(keycode)) {
    RIME_STRUCT(RimeStatus, status);
    if (api_->get_status(session_id, &status)) {
      if (status.is_composing && !status.is_ascii_mode) {
        char schema_id[256] = {0};
        api_->get_current_schema(session_id, schema_id, sizeof(schema_id));
        punct_in_composition =
            LoadZhuyinSpeller(api_, schema_id).alphabet.find((char)keycode) == std::string::npos;
      }
      api_->free_status(&status);
    }
  }

  // 注音組字中按 Backspace：游標前的音節已打聲調（成字）就整個字刪掉，還在拼的才一次刪一鍵
  size_t backspaces = 1;
  if (keycode == key::kBackSpace && !(mask & (mod::kRelease | other_mod_mask | mod::kShift))) {
    RIME_STRUCT(RimeStatus, status);
    if (api_->get_status(session_id, &status)) {
      if (status.is_composing) {
        ++Stats(session_id).backspaces;
        SaveStats();
      } else if (last_commit_ms_ && base::MonotonicMs() - last_commit_ms_ <= 10000) {
        // 送出後很快就在應用程式裡刪字：多半是送錯字
        ++Stats(session_id).deleted_after;
        SaveStats();
      }
      const bool zhuyin = status.is_composing && !status.is_ascii_mode && IsZhuyinSchema(status.schema_id);
      // 音節依 Rime 組字的切法（注音之間以空白分開，省略聲調的連打也切得對），取游標前最後一個音節
      RIME_STRUCT(RimeContext, ctx);
      if (zhuyin && api_->get_context(session_id, &ctx)) {
        if (ctx.composition.preedit && ctx.composition.cursor_pos > 0) {
          const std::wstring before =
              utf8::ToWide(std::string(ctx.composition.preedit).substr(0, ctx.composition.cursor_pos));
          backspaces = zhuyin_preview::BackspaceKeys(before);
        }
        api_->free_context(&ctx);
      }
      api_->free_status(&status);
    }
  }

  Bool handled = api_->process_key(session_id, keycode, mask);
  for (size_t i = 1; i < backspaces; ++i)
    api_->process_key(session_id, keycode, mask);
  if (punct_in_composition) {
    RIME_STRUCT(RimeCommit, commit);
    if (api_->get_commit(session_id, &commit)) {
      if (commit.text)
        ss.mixed_text += utf8::ToWide(commit.text);
      api_->free_commit(&commit);
    }
  }
  // 混打中 Rime 不處理的可見字元（例如組字空了之後的空白）也收進混打內容，不直接輸出
  if (!handled && ss.mixed_active() && !(mask & (mod::kRelease | other_mod_mask)) && keycode >= 0x20 &&
      keycode <= 0x7e) {
    ss.mixed_text += (wchar_t)keycode;
    handled = True;
  }
  // 打字中補全：正在組字時，停頓 300ms 後以 Rime 目前的轉換結果續寫；組字結束則清除補全候選
  const bool rescore = rescore_on_ && RescoreProvider();
  if (handled && !release && (PredictionAvailable() || TypoAvailable() || rescore)) {
    bool composing = false;
    RIME_STRUCT(RimeStatus, status);
    if (api_->get_status(session_id, &status)) {
      composing = status.is_composing && !status.is_ascii_mode;
      api_->free_status(&status);
    }
    if (composing && ((while_typing_ && PredictionAvailable()) || TypoAvailable() || rescore)) {
      ScheduleCompletion(id, kCompletionDelayMs);
    } else if (composing && prediction_mode_) {
      // 未開啟打字中補全：開始打字後，送出後留下的下一詞預測已不適用，直接清掉
      prediction_mode_ = false;
      prediction_->Cancel();
    } else if (!composing) {
      CancelCompletion();
    }
  }
  // vim_mode：Esc 或 Ctrl+C / Ctrl+[ 回到命令模式時切成英文（只看按下）
  if (!handled && !release) {
    const bool vim_back = keycode == key::kEscape ||
                          ((mask & mod::kControl) && (keycode == 'c' || keycode == 'C' ||
                                                      keycode == key::kBracketLeft));
    if (vim_back && api_->get_option(session_id, "vim_mode") &&
        !api_->get_option(session_id, "ascii_mode"))
      api_->set_option(session_id, "ascii_mode", True);
  }
  return {!!handled, true};
}

bool Controller::HandleMixedInput(uint64_t id, SessionState& ss, int keycode, int mask, bool* respond) {
  const RimeSessionId session_id = ss.session_id;
  const bool release = (mask & mod::kRelease) != 0;
  const bool is_shift = keycode == key::kShiftL || keycode == key::kShiftR;
  const bool other_mods = (mask & (mod::kControl | mod::kAlt | mod::kSuper)) != 0;
  bool composing = false;
  RIME_STRUCT(RimeStatus, status);
  if (api_->get_status(session_id, &status)) {
    composing = !!status.is_composing;
    api_->free_status(&status);
  }
  // 吃掉按鍵時一定要回應完整的目前狀態：TSF 讀到空的回應會當成組字結束，把組字清掉
  *respond = true;
  // 送出混打內容與目前的組字，回到一般輸入
  auto commit_all = [&]() {
    std::wstring text = ss.mixed_text;
    if (composing)
      text += TakeComposition(api_, session_id);
    ss.mixed_text.clear();
    ss.mixed_english = false;
    ss.mixed_commit += text;
    if (prediction_mode_ || completion_active_)
      ExitPredictionMode(id);
  };

  // Shift（左右皆可）單獨按下再放開：切換中英。組字中或混打中才由這裡處理，其餘交給 Rime 照舊切換
  if (!release)
    mixed_shift_tap_ = is_shift && !other_mods;
  if (release && is_shift && mixed_shift_tap_) {
    mixed_shift_tap_ = false;
    if (!composing && !ss.mixed_active())
      return false;
    // 英文段不切換 Rime 的 ascii_mode：英文字母都由這裡處理；切成英數會讓應用程式
    // 看到輸入模式改變，有些程式因此把組字藏起來，直到切回中文
    if (ss.mixed_english) {
      ss.mixed_english = false;
    } else {
      if (composing) {
        ss.mixed_text += TakeComposition(api_, session_id);
        if (prediction_mode_ || completion_active_)
          ExitPredictionMode(id);
      }
      ss.mixed_english = true;
    }
    // Rime 只收到這次 Shift 的按下：送一個無作用的鍵，重設它記住的 Shift 狀態
    api_->process_key(session_id, key::kVoidSymbol, 0);
    return true;
  }
  if (!ss.mixed_active())
    return false;
  // 混打中的 Shift 不交給 Rime，免得 Shift+字母（大寫）之後 Rime 自己切換中英
  if (is_shift)
    return true;
  const bool editing_key = keycode == key::kReturn || keycode == key::kKPEnter ||
                           keycode == key::kBackSpace || keycode == key::kEscape;
  const bool printable = keycode >= 0x20 && keycode <= 0x7e && !other_mods;
  if (release)  // 按下時由這裡處理的鍵，放開也吃掉
    return ss.mixed_english && (printable || editing_key);

  if (keycode == key::kReturn || keycode == key::kKPEnter) {
    commit_all();
    return true;
  }
  if (keycode == key::kEscape) {
    api_->clear_composition(session_id);
    ss.mixed_text.clear();
    ss.mixed_english = false;
    if (prediction_mode_ || completion_active_)
      ExitPredictionMode(id);
    return true;
  }
  if (ss.mixed_english || !composing) {
    if (keycode == key::kBackSpace) {
      PopChar(ss.mixed_text);
      // 刪光了：結束混打，停在目前的中英模式
      if (ss.mixed_text.empty())
        ss.mixed_english = false;
      return true;
    }
    if (ss.mixed_english && printable) {
      ss.mixed_text += (wchar_t)keycode;
      return true;
    }
    // 游標移動等按鍵：先送出混打內容，按鍵再交給應用程式
    const bool leaving_key = keycode == key::kLeft || keycode == key::kRight || keycode == key::kUp ||
                             keycode == key::kDown || keycode == key::kHome || keycode == key::kEnd ||
                             keycode == key::kPageUp || keycode == key::kPageDown ||
                             keycode == key::kDelete ||
                             (keycode == key::kTab && !prediction_->HasCandidates());
    if (leaving_key || (other_mods && keycode < 0xff00)) {
      commit_all();  // 由之後一般流程的回應送出
      return false;
    }
  }
  return false;
}

// 框住第 index 個音節：前面的字照目前顯示的樣子確定下來，Rime 的候選就只針對這個字；
// 反白停在目前顯示的字。靠的是最後一次整句轉換的快取（每個音節一個字）
bool Controller::FocusSyllable(SessionState& ss, int index) {
  const RimeSessionId session_id = ss.session_id;
  const char* raw = api_->get_input(session_id);
  const std::string input = raw ? raw : "";
  // 每個字對應的按鍵數，依整句轉換時 Rime 的切法（省略聲調的連打也對得上）
  const std::vector<std::wstring> units = ss.preview_units;
  const std::vector<size_t> lens = ss.preview_lens;
  if (index < 0 || index >= (int)lens.size() || units.size() != lens.size() || ss.preview_input != input)
    return false;
  size_t end = 0;
  for (int i = 0; i <= index; ++i)
    end += lens[i];

  // 找候選清單裡符合條件的候選序號
  auto find_candidate = [&](auto&& match) {
    int found = -1, idx = 0;
    RimeCandidateListIterator it = {0};
    if (api_->candidate_list_begin(session_id, &it)) {
      while (api_->candidate_list_next(&it) && idx < 300) {
        if (it.candidate.text && match(utf8::ToWide(it.candidate.text)))
          found = idx;
        ++idx;
      }
      api_->candidate_list_end(&it);
    }
    return found;
  };

  api_->clear_composition(session_id);
  api_->set_input(session_id, input.c_str());
  api_->set_caret_pos(session_id, end);
  int pos = 0;
  while (pos < index) {
    int best_len = 0;
    const int best = find_candidate([&](const std::wstring& text) {
      const int len = (int)zhuyin_preview::SplitChars(text).size();
      if (len > best_len && len <= index - pos && text == zhuyin_preview::Join(units, pos, pos + len)) {
        best_len = len;
        return true;
      }
      return false;
    });
    if (best < 0 || !api_->select_candidate(session_id, best)) {
      // 對不上：把整句恢復原狀
      api_->clear_composition(session_id);
      api_->set_input(session_id, input.c_str());
      ss.focus = -1;
      return false;
    }
    pos += best_len;
  }
  bool first = true;
  const int shown = find_candidate([&](const std::wstring& text) {
    const bool hit = first && text == units[index];
    if (hit)
      first = false;
    return hit;
  });
  if (shown > 0)
    api_->highlight_candidate(session_id, shown);
  ss.focus_hl = shown > 0 ? shown : 0;
  ss.focus = index;
  ss.focus_input = input;
  ss.focus_caret = end;
  return true;
}

bool Controller::HandleZhuyinFocus(uint64_t id, SessionState& ss, int keycode, int mask) {
  const bool nav = keycode == key::kLeft || keycode == key::kRight || keycode == key::kHome ||
                   keycode == key::kEnd;
  if (!nav || (mask & (mod::kControl | mod::kAlt | mod::kSuper | mod::kShift)))
    return false;
  const RimeSessionId session_id = ss.session_id;
  RIME_STRUCT(RimeStatus, status);
  bool zhuyin = false;
  if (api_->get_status(session_id, &status)) {
    zhuyin = status.is_composing && !status.is_ascii_mode && IsZhuyinSchema(status.schema_id);
    api_->free_status(&status);
  }
  if (!zhuyin)
    return false;
  if (mask & mod::kRelease)
    return ss.focus >= 0;

  const char* raw = api_->get_input(session_id);
  const std::string input = raw ? raw : "";
  // 字數依整句轉換時記下的切法；對不上（例如句中插入過字）就交回 Rime 原本的游標移動
  if (ss.preview_input != input || ss.preview_lens.empty())
    return false;
  const int count = (int)ss.preview_lens.size();
  int target;
  if (keycode == key::kLeft) {
    target = ss.focus < 0 ? count - 1 : (std::max)(0, ss.focus - 1);
  } else if (keycode == key::kHome) {
    target = 0;
  } else if (ss.focus < 0) {
    return false;  // 本來就在句尾
  } else if (keycode == key::kEnd || ss.focus + 1 >= count) {
    // 回到句尾：取消框選，接著打字
    api_->set_caret_pos(session_id, input.size());
    ss.focus = -1;
    return true;
  } else {
    target = ss.focus + 1;
  }
  if (!FocusSyllable(ss, target))
    return false;  // 對不上（例如省略聲調的連打），交回 Rime 原本的游標移動
  if (!ss.focus_used) {
    ss.focus_used = true;
    ++Stats(session_id).focus_uses;
    SaveStats();
  }
  return true;
}

bool Controller::SelectCandidate(uint64_t id, size_t index) {
  SessionState* ss = frontend_->Session(id);
  if (!ss)
    return false;
  UpdateContextKey(id);
  // 預測模式中：Rime 候選之後的是 LLM 候選
  if (prediction_mode_ && prediction_->HasCandidates()) {
    RIME_STRUCT(RimeContext, ctx);
    size_t rime_count = 0;
    if (api_->get_context(ss->session_id, &ctx)) {
      rime_count = ctx.menu.num_candidates;
      api_->free_context(&ctx);
    }
    if (index >= rime_count && CommitPrediction(id, index - rime_count))
      return true;
  }
  CancelCompletion();
  api_->select_candidate_on_current_page(ss->session_id, index);
  return false;
}

void Controller::CommitComposition(uint64_t id) {
  SessionState* ss = frontend_->Session(id);
  if (!ss)
    return;
  if (ss->mixed_active()) {
    // 混打內容連同組字一起，在下一次回應時送出
    ss->mixed_commit += ss->mixed_text + TakeComposition(api_, ss->session_id);
    ss->mixed_text.clear();
    ss->mixed_english = false;
  } else {
    api_->commit_composition(ss->session_id);
  }
}

void Controller::ClearComposition(uint64_t id) {
  SessionState* ss = frontend_->Session(id);
  if (!ss)
    return;
  ss->mixed_text.clear();
  ss->mixed_english = false;
  api_->clear_composition(ss->session_id);
}

void Controller::FocusOut(uint64_t id) {
  if (prediction_mode_)
    ExitPredictionMode(id);
}

// ---------------------------------------------------------------------------
// 回應：送出的文字與組字區

void Controller::RecordCommit(const std::wstring& text) {
  if (history_)
    history_->AddText(text, g_dev_console);
  if (personal_)
    personal_->Record(history_ ? history_->GetActiveKey() : L"", text);
}

std::vector<std::wstring> Controller::TakeCommits(uint64_t id) {
  std::vector<std::wstring> commits;
  SessionState* state = frontend_->Session(id);
  if (!state)
    return commits;
  SessionState& ss = *state;
  // 待送出的 LLM 候選（混打中則接在混打內容後面，等 Enter 一起送出）
  if (!pending_commit_.empty()) {
    if (ss.mixed_active()) {
      ss.mixed_text += pending_commit_;
    } else {
      commits.push_back(pending_commit_);
      CountCommit(ss, pending_commit_);
    }
    pending_commit_.clear();
  }
  // 中英混打結束：整段送出
  if (!ss.mixed_commit.empty()) {
    commits.push_back(ss.mixed_commit);
    RecordCommit(ss.mixed_commit);
    CountCommit(ss, ss.mixed_commit, true);
    ss.mixed_commit.clear();
  }
  RIME_STRUCT(RimeCommit, commit);
  if (ss.mixed_active() && api_->get_commit(ss.session_id, &commit)) {
    // 混打中 Rime 送出的字（選字、標點）收進混打內容
    if (commit.text)
      ss.mixed_text += utf8::ToWide(commit.text);
    api_->free_commit(&commit);
  } else if (api_->get_commit(ss.session_id, &commit)) {
    const std::wstring text = commit.text ? utf8::ToWide(commit.text) : std::wstring();
    api_->free_commit(&commit);
    commits.push_back(text);
    if (!text.empty()) {
      CountCommit(ss, text);
      RecordCommit(text);
      LOG(INFO) << "[LLM] User committed text";
      // 送出有意義的內容（不是只有標點或符號）才預測下一個詞，避免退出後打標點又跳出候選
      if (after_commit_ && PredictionAvailable() && !prediction_mode_ && HasMeaningfulContent(text)) {
        Log(L"[LLM] Detected user commit, entering LLM prediction mode");
        prediction_mode_ = true;
        TriggerPrediction(id);
      }
    }
  }
  return commits;
}

void Controller::UpdateComposition(uint64_t id, bool composing, const RimeContext* ctx) {
  SessionState* state = frontend_->Session(id);
  if (!state)
    return;
  SessionState& ss = *state;
  if (!composing) {
    ss.preview_input.clear();
    ss.preview_units.clear();
    ss.preview_lens.clear();
    ss.focus = -1;
    // 沒送出就結束組字（例如 Esc）：這次不計
    if (!ss.mixed_active()) {
      ss.choice_changed = ss.llm_offered = false;
      ss.focus_used = ss.recommend_offered = false;
      ss.default_text.clear();
      ss.default_zhuyin.clear();
    }
    return;
  }
  if (!ctx)
    return;
  // 選字統計：反白離開第一候選（框選時是離開原本顯示的字）就算換字
  const int base_hl = ss.focus >= 0 ? ss.focus_hl : 0;
  if (ctx->menu.num_candidates > 0 && ctx->menu.highlighted_candidate_index != base_hl)
    ss.choice_changed = true;
  if (prediction_mode_ && prediction_->HasCandidates())
    ss.llm_offered = true;
  // 選字紀錄：還沒換字、游標在最後時，記下 Rime 的預設轉換與注音
  if (choice_log_ && !ss.choice_changed && ctx->commit_text_preview) {
    const char* input = api_->get_input(ss.session_id);
    if (input && api_->get_caret_pos(ss.session_id) == std::strlen(input)) {
      std::wstring preview = utf8::ToWide(ctx->commit_text_preview);
      while (!preview.empty() && preview.back() < 0x80)
        preview.pop_back();
      ss.default_text = preview;
      ss.default_zhuyin = ComposingZhuyin(ss.session_id);
    }
  }
}

bool Controller::PreviewPreedit(uint64_t id, const RimeContext& ctx, bool ascii_mode, Preedit* out) {
  SessionState* state = frontend_->Session(id);
  // 西文模式（Shift 的 inline_ascii）下組字是英文按鍵，不能當注音轉換
  if (!state || !ctx.commit_text_preview || ascii_mode)
    return false;
  SessionState& ss = *state;
  const RimeSessionId session_id = ss.session_id;
  char schema_id[256] = {0};
  api_->get_current_schema(session_id, schema_id, sizeof(schema_id));
  const char* input = api_->get_input(session_id);
  const int hl = ctx.menu.highlighted_candidate_index;
  const char* cand = hl >= 0 && hl < ctx.menu.num_candidates ? ctx.menu.candidates[hl].text : nullptr;
  // 逐字選字：選好了（候選沒了、游標跑回句尾）或輸入改變，就取消框選
  const size_t caret = api_->get_caret_pos(session_id);
  if (ss.focus >= 0 &&
      (ctx.menu.num_candidates == 0 || ss.focus_input != (input ? input : "") || ss.focus_caret != caret))
    ss.focus = -1;
  ZhuyinPreview pv = BuildZhuyinPreview(ctx.commit_text_preview,
                                        ctx.composition.preedit ? ctx.composition.preedit : "",
                                        input ? input : "", caret, cand ? cand : "", hl,
                                        LoadZhuyinSpeller(api_, schema_id), ss.preview_input,
                                        ss.preview_units, ss.preview_lens, ss.focus >= 0);
  // without a chosen word the whole preedit is the selection
  if (pv.sel_start == pv.sel_end) {
    pv.sel_start = 0;
    pv.sel_end = (int)pv.text.size();
  }
  out->text = pv.text;
  out->sel_start = pv.sel_start;
  out->sel_end = pv.sel_end;
  out->cursor = pv.cursor;
  return true;
}

Preedit Controller::WithMixedText(const SessionState& ss, const Preedit* rime) {
  Preedit out;
  const std::wstring rime_text = rime ? rime->text : std::wstring();
  out.text = ss.mixed_text + rime_text;
  const int offset = (int)ss.mixed_text.size();
  const int total = (int)out.text.size();
  if (!rime || rime->cursor < 0 || rime_text.empty()) {
    out.sel_start = 0;
    out.sel_end = out.cursor = total;
  } else if (rime->sel_start == 0 && rime->sel_end == (int)rime_text.size()) {
    // 選取整段組字時，混打內容也一起算，不另外標示
    out.sel_start = 0;
    out.sel_end = total;
    out.cursor = rime->cursor + offset;
  } else {
    out.sel_start = rime->sel_start + offset;
    out.sel_end = rime->sel_end + offset;
    out.cursor = rime->cursor + offset;
  }
  return out;
}

bool Controller::ShowingPredictions() const {
  return prediction_mode_ && prediction_->HasCandidates();
}

PredictionSet Controller::Predictions() const {
  return prediction_->Snapshot();
}

// ---------------------------------------------------------------------------
// 預測

void Controller::TriggerPrediction(uint64_t id,
                                   const std::wstring& current_input,
                                   unsigned delay_ms,
                                   const std::wstring& completion_prefix,
                                   const std::wstring& zhuyin,
                                   bool complete) {
  SessionState* ss = frontend_->Session(id);
  if (!ss)
    return;
  const bool llm_available = llm_provider_ && llm_provider_->IsAvailable();
  const bool correct = TypoAvailable() && !zhuyin.empty() && !completion_prefix.empty();
  // 推薦：打字中（有 Rime 轉換結果）才做
  const bool rescore = rescore_on_ && !completion_prefix.empty() && RescoreProvider();
  if (!llm_available && !personal_ && !correct && !rescore) {
    LOG(WARNING) << "[LLM] neither LLM provider nor personal lexicon is available";
    return;
  }

  PredictionRequest request;
  request.tag = id;
  // 個人詞庫（查記憶體，不到 1ms）：打字中補全 → 以 Rime 轉換結果開頭的常用詞；
  // 送出後 → 目前視窗最後一個詞之後最常接的詞
  if (personal_ && complete) {
    request.personal =
        completion_prefix.empty()
            ? personal_->NextAfter(history_ ? history_->GetActiveKey() : std::wstring(), personal_max_)
            : personal_->CompleteFrom(completion_prefix, personal_max_);
    // 補全候選若和目前的字完全相同，沒有意義
    request.personal.erase(
        std::remove(request.personal.begin(), request.personal.end(), completion_prefix),
        request.personal.end());
  }
  if (!llm_available && request.personal.empty() && !correct && !rescore)
    return;

  // 前文：目前視窗最近的前文；中英混打中尚未送出的部分也是前文
  if (history_)
    request.history = history_->GetRecentContext(context_max_chars_);
  request.history += ss->mixed_text;
  request.prefix = completion_prefix;
  request.current_input = current_input;
  request.predict = llm_available && complete;
  request.correct = correct;
  request.rescore = rescore;
  request.zhuyin = zhuyin;
  // 校正只看組字區的內容：組字區裡已確定的部分（混打、標點）當前文，不帶之前送出的文字
  request.typo_context = ss->mixed_text;
  {
    std::lock_guard<std::mutex> lock(typo_prompt_mutex_);
    request.typo_prompt = typo_prompt_;
  }
  request.delay_ms = delay_ms;
  prediction_->Request(std::move(request));
}

bool Controller::RescoreInput(uint64_t id,
                              uint64_t seq,
                              std::vector<std::wstring>* units,
                              std::vector<std::vector<std::wstring>>* homophones) {
  // 同音字要用 Rime：在服務端的鎖下查（引擎這時還沒拿推理鎖，不會和等推理的請求互相卡住）
  std::lock_guard<std::mutex> api_lock(frontend_->ApiMutex());
  if (!prediction_->IsCurrent(seq))
    return false;
  const SessionState* ss = frontend_->Session(id);
  if (!ss)
    return false;
  const char* raw = api_->get_input(ss->session_id);
  const std::string input = raw ? raw : "";
  char schema_id[256] = {0};
  api_->get_current_schema(ss->session_id, schema_id, sizeof(schema_id));
  if (input.empty() || ss->preview_input != input || ss->focus >= 0 ||
      ss->preview_lens.size() != ss->preview_units.size() || !IsZhuyinSchema(schema_id))
    return false;
  *units = ss->preview_units;
  size_t offset = 0;
  for (size_t len : ss->preview_lens) {
    homophones->push_back(homophones_->Find(schema_id, input.substr(offset, len)));
    offset += len;
  }
  return true;
}

void Controller::OnPredictionUpdate(uint64_t id, uint64_t seq, const PredictionSet& set) {
  // 刷新候選窗必須在服務端的鎖下進行：按鍵處理執行緒同時在用 librime 與候選窗
  std::lock_guard<std::mutex> api_lock(frontend_->ApiMutex());
  if (!prediction_->IsCurrent(seq))
    return;
  // 選字統計：這次組字第一次出現推薦（統計只在服務端的鎖下讀寫）
  SessionState* ss = frontend_->Session(id);
  if (set.recommends > 0 && ss && !ss->recommend_offered) {
    ss->recommend_offered = true;
    ++Stats(ss->session_id).recommend_offered;
    SaveStats();
  }
  frontend_->Refresh(id);
}

void Controller::ExitPredictionMode(uint64_t id) {
  prediction_mode_ = false;
  completion_active_ = false;
  prediction_->Cancel();  // 丟棄仍在進行中的預測
  // 強制收起候選欄
  frontend_->HideCandidates();
  frontend_->Refresh(id);
  Log(L"[LLM] 退出LLM预测模式");
}

void Controller::ScheduleCompletion(uint64_t id, unsigned delay_ms) {
  SessionState* ss = frontend_->Session(id);
  if (!ss)
    return;
  const bool predict = PredictionAvailable();
  const bool rescore = rescore_on_ && RescoreProvider();
  if (!predict && !TypoAvailable() && !rescore)
    return;

  // Rime 若此刻送出會得到的文字（整句轉換）；沒有時退回第一個候選
  std::wstring preview;
  RIME_STRUCT(RimeContext, ctx);
  if (api_->get_context(ss->session_id, &ctx)) {
    if (ctx.commit_text_preview && *ctx.commit_text_preview)
      preview = utf8::ToWide(ctx.commit_text_preview);
    else if (ctx.menu.num_candidates > 0 && ctx.menu.candidates[0].text)
      preview = utf8::ToWide(ctx.menu.candidates[0].text);
    api_->free_context(&ctx);
  }
  // 注音：往回選字時轉換結果後面會接著還沒轉換的按鍵，只把游標前轉好的中文當前文
  {
    char schema_id[256] = {0};
    api_->get_current_schema(ss->session_id, schema_id, sizeof(schema_id));
    if (IsZhuyinSchema(schema_id)) {
      while (!preview.empty() && preview.back() < 0x80)
        preview.pop_back();
    }
  }
  if (preview.empty()) {
    CancelCompletion();
    return;
  }

  // 續寫：開啟智慧預測且（打字中補全或按 ` 鍵手動觸發，delay_ms 為 0）；
  // 整句校正：開啟 LLM 整句校正（與智慧預測、Rime 容錯各自獨立）
  const bool complete = predict && (while_typing_ || delay_ms == 0);
  const std::wstring zhuyin = TypoAvailable() ? ComposingZhuyin(ss->session_id) : std::wstring();
  if (!complete && zhuyin.empty() && !rescore) {
    CancelCompletion();
    return;
  }

  // 舊候選（上一個鍵的補全或送出後的下一詞預測）已不適用，先清掉
  prediction_->Cancel();
  prediction_mode_ = true;
  completion_active_ = true;
  Log(L"[LLM] 输入中补全（" + std::to_wstring(delay_ms) + L"ms 后）: " + preview +
      (zhuyin.empty() ? L"" : L"，注音: " + zhuyin));
  TriggerPrediction(id, L"", delay_ms, preview, zhuyin, complete);
}

std::wstring Controller::ComposingZhuyin(RimeSessionId session_id) {
  bool zhuyin_schema = false;
  RIME_STRUCT(RimeStatus, status);
  if (api_->get_status(session_id, &status)) {
    zhuyin_schema = IsZhuyinSchema(status.schema_id);
    api_->free_status(&status);
  }
  const char* raw = zhuyin_schema ? api_->get_input(session_id) : nullptr;
  if (!raw || !*raw)
    return L"";
  // 往回選字時只取游標前的部分（與送給模型的前文一致）；游標在最前面時 Rime 轉換整段
  std::string before(raw);
  const size_t caret = api_->get_caret_pos(session_id);
  if (caret > 0 && caret < before.size())
    before.resize(caret);
  // 注音方案（大千式）的按鍵 → 注音符號；聲調之後斷開，方便模型分辨音節
  static const char kKeys[] = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-";
  static const wchar_t kSymbols[] = L"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ";
  static const char kToneKeys[] = "6347";
  static const wchar_t kTones[] = L"ˊˇˋ˙";
  std::wstring zhuyin;
  for (const char* p = before.c_str(); *p; ++p) {
    if (const char* k = std::strchr(kKeys, *p)) {
      zhuyin += kSymbols[k - kKeys];
    } else if (const char* t = std::strchr(kToneKeys, *p)) {
      zhuyin += kTones[t - kToneKeys];
      zhuyin += L' ';
    } else if (*p == ' ' || *p == '\'') {
      if (!zhuyin.empty() && zhuyin.back() != L' ')
        zhuyin += L' ';
    } else {
      return L"";  // 不是大千鍵位（例如其他注音鍵盤配置），不校正
    }
  }
  while (!zhuyin.empty() && zhuyin.back() == L' ')
    zhuyin.pop_back();
  return zhuyin;
}

void Controller::CancelCompletion() {
  if (!completion_active_)
    return;
  completion_active_ = false;
  prediction_mode_ = false;
  prediction_->Cancel();  // 讓尚未完成的補全請求作廢
}

bool Controller::ConfirmText(SessionState& ss, const std::wstring& desired) {
  const char* raw = api_->get_input(ss.session_id);
  const std::string input = raw ? raw : "";
  const std::vector<std::wstring> units = zhuyin_preview::SplitChars(desired);
  if (input.empty() || ss.preview_input != input || ss.preview_lens.size() != units.size())
    return false;
  if (!SelectText(api_, ss.session_id, input, units))
    return false;
  ss.focus = -1;
  return true;
}

bool Controller::CommitPrediction(uint64_t id, size_t index) {
  SessionState* state = frontend_->Session(id);
  if (!state)
    return false;
  SessionState& ss = *state;
  std::wstring selected;
  bool correction = false, recommend = false;
  if (!prediction_->Take(index, &selected, &recommend, &correction))
    return false;
  // 推薦：用 Rime 逐段選字把整句改成推薦的樣子，留在組字區（送出時 Rime 照常學習）
  if (recommend && ConfirmText(ss, selected)) {
    ++Stats(ss.session_id).recommend_used;
    SaveStats();
    ss.choice_changed = true;
    completion_active_ = false;
    prediction_mode_ = false;
    Log(L"[LLM] 套用推薦: " + selected);
    return true;
  }
  {
    ChoiceStats& stats = Stats(ss.session_id);
    ++stats.llm_used;
    if (correction)
      ++stats.corrections_used;
    SaveStats();
    ss.llm_committed = true;
    ss.correction_committed = correction;
  }
  Log(L"[LLM] 选择LLM候选词: " + std::to_wstring(index + 1) + L". " + selected);

  // 往回選字（游標不在最後）：候選取代游標前的部分，留在組字區；游標後的按鍵留給 Rime 繼續編輯
  {
    const char* raw = api_->get_input(ss.session_id);
    const std::string input = raw ? raw : "";
    const size_t caret = api_->get_caret_pos(ss.session_id);
    char schema_id[256] = {0};
    api_->get_current_schema(ss.session_id, schema_id, sizeof(schema_id));
    if (IsZhuyinSchema(schema_id) && caret > 0 && caret < input.size()) {
      ss.mixed_text += selected;
      api_->clear_composition(ss.session_id);
      api_->set_input(ss.session_id, input.substr(caret).c_str());
      completion_active_ = false;
      prediction_mode_ = false;
      return true;
    }
  }

  // 補全候選已包含 Rime 的轉換結果，丟棄組字後整段送出
  api_->clear_composition(ss.session_id);
  RecordCommit(selected);
  pending_commit_ = selected;
  completion_active_ = false;

  // 繼續預測下一個詞（可用 llm/predict_after_commit 關閉；關閉智慧預測時只有整句校正，不接著預測）
  const bool predict_next = after_commit_ && PredictionAvailable();
  prediction_mode_ = predict_next;
  if (predict_next)
    TriggerPrediction(id);
  return true;
}

// ---------------------------------------------------------------------------
// 選字統計與紀錄

std::string Controller::ChoiceProfile(RimeSessionId session_id) {
  char schema[256] = {0};
  api_->get_current_schema(session_id, schema, sizeof(schema));
  // 方案裡的設定（重新部署才會變，快取起來）
  std::string& schema_flags = schema_flags_[schema];
  if (schema_flags.empty()) {
    std::vector<std::string> parts;
    RimeConfig config = {NULL};
    if (*schema && api_->schema_open(schema, &config)) {
      char buf[256] = {0};
      if (api_->config_get_string(&config, "grammar/language", buf, sizeof(buf) - 1) && *buf)
        parts.push_back("語言模型");
      Bool on = False;
      if (api_->config_get_bool(&config, "translator/enable_correction", &on) && on)
        parts.push_back("Rime 容錯");
      buf[0] = 0;
      if (api_->config_get_string(&config, "translator/dictionary", buf, sizeof(buf) - 1) &&
          std::string(buf) == "terra_pinyin.personal")
        parts.push_back("個人詞庫排序");
      api_->config_close(&config);
    }
    schema_flags = "=";  // 已查過（可能沒有任何設定）
    for (const auto& p : parts)
      schema_flags += (schema_flags.size() > 1 ? "、" : "") + p;
  }
  std::string settings = schema_flags.substr(1);
  auto add = [&](bool on, const char* name) {
    if (on)
      settings += (settings.empty() ? "" : "、") + std::string(name);
  };
  add(rescore_on_ && RescoreProvider(), "推薦");
  add(TypoAvailable(), "LLM 校正");
  add(PredictionAvailable() && while_typing_, "智慧預測");
  if (settings.empty())
    settings = "（無）";
  settings = std::string(*schema ? schema : "?") + "｜" + settings;
  const std::string key = ProfileKey(options_.version, settings);
  choice_store_->AddProfile(key, options_.version, options_.build_time, options_.build_subject, settings);
  return key;
}

ChoiceStats& Controller::Stats(RimeSessionId session_id) {
  return choice_store_->Today(ChoiceProfile(session_id));
}

void Controller::SaveStats() {
  choice_store_->Save();
}

void Controller::CountCommit(SessionState& ss, const std::wstring& text, bool mixed) {
  ChoiceStats& s = Stats(ss.session_id);
  ++s.commits;
  s.chars += (int64_t)text.size();
  if (ss.choice_changed)
    ++s.changed;
  if (ss.llm_offered)
    ++s.llm_offered;
  SaveStats();
  last_commit_ms_ = base::MonotonicMs();
  LogChoice(ss, text, mixed);
  ss.choice_changed = ss.llm_offered = ss.focus_used = ss.recommend_offered = false;
  ss.llm_committed = ss.correction_committed = false;
  ss.default_text.clear();
  ss.default_zhuyin.clear();
}

void Controller::LogChoice(SessionState& ss, const std::wstring& text, bool mixed) {
  if (!choice_log_ || text.empty())
    return;
  ChoiceRecord record;
  record.time = (int64_t)std::time(nullptr);
  record.method = ChoiceMethod(mixed, ss.correction_committed, ss.llm_committed, ss.focus_used,
                               ss.choice_changed);
  // 前文：送出之前的最近文字（有些路徑已先把這次送出的文字加進前文）
  record.context = ChoiceContext(history_ ? history_->GetRecentContext(40 + text.size()) : L"", text);
  char app[256] = {0};
  api_->get_property(ss.session_id, "client_app", app, sizeof(app) - 1);
  record.app = app;
  record.zhuyin = ss.default_zhuyin;
  record.default_text = ss.default_text;
  record.text = text;
  AppendChoiceRecord(options_.user_dir / "personal", record);
}

// ---------------------------------------------------------------------------
// 設定程式的指令

void Controller::PersonalCommand(int command) {
  if (command == 8) {
    choice_store_->Reset();
    return;
  }
  if (!personal_ || !refiner_) {
    const fs::path dir = options_.user_dir / "personal";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "status.txt", std::ios::binary | std::ios::trunc);
    f << "disabled=1\n";
    return;
  }
  switch (command) {
    case 2:
    case 3:
      if (!refiner_->RunAsync(command == 3))
        refiner_->WriteStatus();
      break;
    case 4:
      if (!refiner_->Clear())
        refiner_->WriteStatus();
      break;
    case 6: {
      // 設定程式的「詞庫管理」：套用 personal/edit.dat（加密）後重新匯出
      const fs::path edit = personal_->Dir() / "edit.dat";
      personal_->ApplyEdits(edit);
      std::error_code ec;
      fs::remove(edit, ec);
    }
      [[fallthrough]];
    case 7:
      // 設定程式開啟「注音排序」時：先產生 Rime 詞典，設定程式接著重新部署
      refiner_->ExportRimeDict();
      refiner_->WriteStatus();
      break;
    case 5:
      // 匯出詞彙與精煉規則到 personal/export.dat（加密）
      personal_->ExportTo(personal_->Dir() / "export.dat", 20000);
      refiner_->WriteStatus();
      break;
    default:
      refiner_->WriteStatus();
  }
}

void Controller::LLMTest() {
  // 在背景執行緒做：推理可能要幾百毫秒，不能佔住服務端的鎖卡住所有應用程式的打字
  std::thread([this]() {
    const fs::path dir = options_.user_dir;
    std::string request;
    {
      std::ifstream in(dir / "llm_test_request.txt", std::ios::binary);
      request.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // 第一行是請求編號，其餘是前文（空 = 只查詢狀態）
    const size_t nl = request.find('\n');
    std::string req_id = request.substr(0, nl);
    while (!req_id.empty() && (req_id.back() == '\r' || req_id.back() == ' '))
      req_id.pop_back();
    std::wstring context = nl == std::string::npos ? L"" : utf8::ToWide(request.substr(nl + 1));
    while (!context.empty() && (context.back() == L'\r' || context.back() == L'\n'))
      context.pop_back();

    std::ostringstream out;
    out << "id=" << req_id << "\n";
    {
      std::lock_guard<std::mutex> infer_lock(prediction_->InferMutex());
      out << "model=" << utf8::FromWide(loaded_model_) << "\n";
      if (!llm_provider_ || !llm_provider_->IsAvailable()) {
        out << "status=disabled\n";
      } else if (context.empty()) {
        out << "status=ok\n";
      } else {
        const uint64_t t0 = base::MonotonicMs();
        auto raw = llm_provider_->PredictCandidates(context, L"", 5);
        out << "status=ok\nms=" << base::MonotonicMs() - t0 << "\n";
        for (const auto& c : CleanCandidates(raw, L""))
          out << "cand=" << utf8::FromWide(c) << "\n";
      }
    }
    // 先寫暫存檔再取代，避免設定畫面讀到寫了一半的內容
    const fs::path tmp = dir / "llm_test_response.tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f << out.str();
    }
    std::error_code ec;
    fs::rename(tmp, dir / "llm_test_response.txt", ec);
  }).detach();
}

}  // namespace ime
