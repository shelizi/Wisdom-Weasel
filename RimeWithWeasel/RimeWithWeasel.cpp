#include "stdafx.h"
#include <logging.h>
#include <RimeWithWeasel.h>
#include <StringAlgorithm.hpp>
#include <WeaselConstants.h>
#include <WeaselUtility.h>
#include <FixedWMemStreamBuf.h>
#include "ZhuyinPreview.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <rime_api.h>

// 判断字符是否为分隔符（仅空格/标点），用于决定 commit 是否触发进入 LLM 预测模式
static bool IsSeparatorOrPunctuation(wchar_t ch) {
  if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r')
    return true;
  // 常见中文标点
  if (ch == L'，' || ch == L'。' || ch == L'、' || ch == L'；' ||
      ch == L'：' || ch == L'？' || ch == L'！' || ch == L'…' ||
      ch == L'—' || ch == L'–' || ch == L'（' || ch == L'）' ||
      ch == L'【' || ch == L'】' || ch == L'《' || ch == L'》' ||
      ch == L'“' || ch == L'”' || ch == L'‘' || ch == L'’')
    return true;
  // 常见英文/通用标点
  if (ch == L',' || ch == L'.' || ch == L';' || ch == L':' ||
      ch == L'?' || ch == L'!' || ch == L'-' || ch == L'_' ||
      ch == L'(' || ch == L')' || ch == L'[' || ch == L']' ||
      ch == L'{' || ch == L'}' || ch == L'"' || ch == L'\'' ||
      ch == L'/' || ch == L'\\' || ch == L'*' || ch == L'#' ||
      ch == L'@' || ch == L'$' || ch == L'%' || ch == L'^' ||
      ch == L'&' || ch == L'+' || ch == L'=' || ch == L'~' ||
      ch == L'`' || ch == L'|' || ch == L'<' || ch == L'>')
    return true;
  return false;
}

// 文本是否包含至少一个“有意义”字符（非纯标点/空格），用于避免仅输入符号时误入 LLM 预测模式
static bool CommitHasMeaningfulContent(const std::wstring& text) {
  for (wchar_t ch : text) {
    if (!IsSeparatorOrPunctuation(ch))
      return true;
  }
  return false;
}

// 包含 ContextHistory 的完整定义（需要调用其方法）
#include "../WeaselServer/ContextHistory.h"
#include "../WeaselServer/PersonalLexicon.h"
#include "../WeaselServer/PersonalRefiner.h"
// 包含 LLMProvider 的完整定义
#include "../WeaselServer/LLMProvider.h"
// 包含 DevConsole 的完整定义（需要调用其方法）
#include "../WeaselServer/DevConsole.h"

#define TRANSPARENT_COLOR 0x00000000
#define ARGB2ABGR(value)                                 \
  ((value & 0xff000000) | ((value & 0x000000ff) << 16) | \
   (value & 0x0000ff00) | ((value & 0x00ff0000) >> 16))
#define RGBA2ABGR(value)                                   \
  (((value & 0xff) << 24) | ((value & 0xff000000) >> 24) | \
   ((value & 0x00ff0000) >> 8) | ((value & 0x0000ff00) << 8))
typedef enum { COLOR_ABGR = 0, COLOR_ARGB, COLOR_RGBA } ColorFormat;

#ifdef USE_SHARP_COLOR_CODE
#define HEX_REGEX std::regex("^(0x|#)[0-9a-f]+$", std::regex::icase)
#define TRIMHEAD_REGEX std::regex("0x|#", std::regex::icase)
#else
#define HEX_REGEX std::regex("^0x[0-9a-f]+$", std::regex::icase)
#define TRIMHEAD_REGEX std::regex("0x", std::regex::icase)
#endif
using namespace weasel;
static bool hide_ime_mode_icon = false;

static RimeApi* rime_api;
WeaselSessionId _GenerateNewWeaselSessionId(SessionStatusMap sm, DWORD pid) {
  if (sm.empty())
    return (WeaselSessionId)(pid + 1);
  return (WeaselSessionId)(sm.rbegin()->first + 1);
}

int expand_ibus_modifier(int m) {
  return (m & 0xff) | ((m & 0xff00) << 16);
}

RimeWithWeaselHandler::RimeWithWeaselHandler(UI* ui)
    : m_ui(ui),
      m_active_session(0),
      m_disabled(true),
      m_current_dark_mode(false),
      m_global_ascii_mode(false),
      m_show_notifications_time(1200),
      _UpdateUICallback(NULL),
      m_context_history(nullptr),
      m_dev_console(nullptr),
      m_llm_provider(nullptr),
      m_llm_prediction_mode(false),
      m_pending_llm_commit(L""),
      m_last_grave_key_time(0) {
  rime_api = rime_get_api();
  assert(rime_api);
  m_pid = GetCurrentProcessId();
  uint16_t msbit = 0;
  for (auto i = 31; i >= 0; i--) {
    if (m_pid & (1 << i)) {
      msbit = i;
      break;
    }
  }
  m_pid = (m_pid << (31 - msbit));
  _Setup();
}

RimeWithWeaselHandler::~RimeWithWeaselHandler() {
  _WaitRetired();
  m_show_notifications.clear();
  m_session_status_map.clear();
  m_app_options.clear();
}

bool add_session = false;
void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize);
bool _UpdateUIStyleColor(RimeConfig* config,
                         UIStyle& style,
                         std::string color = "");
void _LoadAppOptions(RimeConfig* config, AppOptionsByAppName& app_options);

void _RefreshTrayIcon(const RimeSessionId session_id,
                      const std::function<void()> _UpdateUICallback) {
  // Dangerous, don't touch
  static char app_name[256] = {0};
  auto ret = rime_api->get_property(session_id, "client_app", app_name,
                                    sizeof(app_name) - 1);
  if (!ret || u8tow(app_name) == std::wstring(L"explorer.exe"))
    auto th = std::make_unique<ScopedThread>([=]() {
      ::Sleep(100);
      if (_UpdateUICallback)
        _UpdateUICallback();
    });
  else if (_UpdateUICallback)
    _UpdateUICallback();
}

void RimeWithWeaselHandler::_Setup() {
  RIME_STRUCT(RimeTraits, weasel_traits);
  std::string shared_dir = wtou8(WeaselSharedDataPath().wstring());
  std::string user_dir = wtou8(WeaselUserDataPath().wstring());
  weasel_traits.shared_data_dir = shared_dir.c_str();
  weasel_traits.user_data_dir = user_dir.c_str();
  weasel_traits.prebuilt_data_dir = weasel_traits.shared_data_dir;
  std::string distribution_name = wtou8(get_weasel_ime_name());
  weasel_traits.distribution_name = distribution_name.c_str();
  weasel_traits.distribution_code_name = WEASEL_CODE_NAME;
  weasel_traits.distribution_version = WEASEL_VERSION;
  weasel_traits.app_name = "rime.weasel";
  std::string log_dir = WeaselLogPath().u8string();
  weasel_traits.log_dir = log_dir.c_str();
  rime_api->setup(&weasel_traits);
  rime_api->set_notification_handler(&RimeWithWeaselHandler::OnNotify, this);
}

void RimeWithWeaselHandler::Initialize() {
  m_disabled = _IsDeployerRunning();
  if (m_disabled) {
    return;
  }

  LOG(INFO) << "Initializing la rime.";
  rime_api->initialize(NULL);
  HANDLE hMutex =
      CreateMutexW(NULL, FALSE, L"Global\\WeaselStartMaintenanceMutex");
  if (hMutex) {
    if (WaitForSingleObject(hMutex, 0) == WAIT_OBJECT_0) {
      if (rime_api->start_maintenance(/*full_check = */ False)) {
        rime_api->join_maintenance_thread();
        m_disabled = true;
      }
      ReleaseMutex(hMutex);
    }
    CloseHandle(hMutex);
  }
  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      _UpdateShowNotifications(&config, true);
      m_current_dark_mode = IsUserDarkMode();
      if (m_current_dark_mode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
    }
    Bool global_ascii = false;
    if (rime_api->config_get_bool(&config, "global_ascii", &global_ascii))
      m_global_ascii_mode = !!global_ascii;
    if (!rime_api->config_get_int(&config, "show_notifications_time",
                                  &m_show_notifications_time))
      m_show_notifications_time = 1200;
    _LoadAppOptions(&config, m_app_options);
    
    // 初始化LLM Provider (注意：此时m_dev_console可能还未初始化)
    // 先释放旧 provider（重新部署时），避免新旧模型同时占用内存，或关闭 LLM 后旧模型仍驻留。
    // 作废排队中的预测，并等进行中的推理结束，避免后台线程用到已释放的模型
    ++m_llm_request_seq;
    std::lock_guard<std::mutex> infer_lock(m_llm_infer_mutex);
    m_typo_llm = nullptr;  // 可能指向 m_llm_provider，先放掉
    m_typo_owned.reset();
    m_llm_provider.reset();
    m_llm_loaded_model.clear();
    // 两种自动触发时机可分别关闭（未设置时默认开启）；关闭后仍可按 ` 键手动触发
    Bool llm_flag = true;
    m_llm_after_commit =
        !rime_api->config_get_bool(&config, "llm/predict_after_commit", &llm_flag) || llm_flag;
    llm_flag = true;
    m_llm_while_typing =
        !rime_api->config_get_bool(&config, "llm/predict_while_typing", &llm_flag) || llm_flag;
    // LLM 整句校正（llm/typo/llm）；Rime 容錯另外設定在注音方案裡，與此無關。
    // 舊設定 llm/typo_correction: llm 視為開啟
    {
      Bool typo_llm = false;
      char legacy[32] = {0};
      if (rime_api->config_get_bool(&config, "llm/typo/llm", &typo_llm))
        m_typo_llm_on = !!typo_llm;
      else
        m_typo_llm_on =
            rime_api->config_get_string(&config, "llm/typo_correction", legacy, sizeof(legacy) - 1) &&
            strcmp(legacy, "llm") == 0;
    }
    // 前文：每个窗口各自一份；只给模型最后 max_chars 个字；窗口闲置 idle_minutes 后旧前文失效
    int llm_int = 0;
    m_llm_context_max_chars =
        rime_api->config_get_int(&config, "llm/context/max_chars", &llm_int) && llm_int > 0
            ? (size_t)llm_int
            : 100;
    // 個人詞庫：從送出的文字學習常用詞與接續（預設開啟；資料以 DPAPI 加密存在使用者資料夾）
    {
      Bool personal_enabled = true;
      if (!rime_api->config_get_bool(&config, "llm/personal/enabled", &personal_enabled))
        personal_enabled = true;
      int personal_int = 0;
      m_personal_max =
          rime_api->config_get_int(&config, "llm/personal/max_candidates", &personal_int) &&
                  personal_int >= 0
              ? (size_t)(std::min)(personal_int, 5)
              : 3;
      if (personal_enabled && !m_personal) {
        _WaitRetired();  // 上一份還在存檔時先等它，避免同時讀寫檔案（已要求停止，通常很快）
        m_personal = std::make_unique<PersonalLexicon>(WeaselUserDataPath() / L"personal");
        m_personal->Load();
      } else if (!personal_enabled && m_personal) {
        _RetirePersonal();  // 背景停止精煉並存檔，不在這裡等
      }
      if (m_personal) {
        personal_int = 0;
        if (rime_api->config_get_int(&config, "llm/personal/half_life_days", &personal_int) &&
            personal_int > 0)
          m_personal->SetHalfLifeDays(personal_int);
        Bool keep_log = true;
        if (!rime_api->config_get_bool(&config, "llm/personal/keep_raw_log", &keep_log))
          keep_log = true;
        m_personal->SetKeepRawLog(!!keep_log);

        // 定時精煉：預設每 1 天（0 = 只手動）。精煉用的模型由設定程式從「語言模型」清單選用後
        // 寫到 llm/personal/refine/*（type 為 llamacpp 或 openai；沒選時只做統計整理）
        PersonalRefiner::Config refine;
        char buf[1024] = {0};
        if (rime_api->config_get_string(&config, "llm/personal/refine/interval_days", buf,
                                        sizeof(buf)))
          refine.interval_days = (std::max)(0.0, atof(buf));
        auto read_string = [&](const char* key) {
          char value[2048] = {0};
          return rime_api->config_get_string(&config, key, value, sizeof(value))
                     ? std::string(value)
                     : std::string();
        };
        refine.type = read_string("llm/personal/refine/type");
        refine.name = read_string("llm/personal/refine/name");
        refine.api_url = read_string("llm/personal/refine/api_url");
        refine.api_key = read_string("llm/personal/refine/api_key");
        refine.model = read_string("llm/personal/refine/model");
        refine.model_path = read_string("llm/personal/refine/model_path");
        std::string refine_model_type = read_string("llm/personal/refine/model_type");
        std::transform(refine_model_type.begin(), refine_model_type.end(),
                       refine_model_type.begin(), ::tolower);
        refine.instruct = refine_model_type != "base";
        refine.disable_thinking = read_string("llm/personal/refine/disable_thinking") == "true";
        {
          int think_tokens = 2048;
          if (rime_api->config_get_int(&config, "llm/personal/refine/think_tokens", &think_tokens))
            refine.think_tokens = (std::max)(0, think_tokens);
        }
        // 舊設定（只有 api_url）視為 OpenAI 相容 API
        if (refine.type.empty() && !refine.api_url.empty() &&
            read_string("llm/personal/refine/profile").empty())
          refine.type = "openai";
        // 讓常打的詞影響注音選字排序（設定程式負責修改方案；這裡負責產生詞典）
        Bool rime_boost = false;
        refine.rime_boost =
            rime_api->config_get_bool(&config, "llm/personal/rime_boost", &rime_boost) && rime_boost;
        refine.rime_dict_path = (WeaselUserDataPath() / L"terra_pinyin.personal.dict.yaml").wstring();
        refine.essay_path = (WeaselSharedDataPath() / L"essay.txt").wstring();
        {
          wchar_t exe[MAX_PATH] = {0};
          GetModuleFileNameW(NULL, exe, _countof(exe));
          refine.deployer_path =
              (std::filesystem::path(exe).parent_path() / L"WeaselDeployer.exe").wstring();
        }
        // 本機精煉沿用預測的 GPU / 執行緒設定
        int llama_int = 0;
        if (rime_api->config_get_int(&config, "llm/llamacpp/n_gpu_layers", &llama_int))
          refine.n_gpu_layers = llama_int;
        if (rime_api->config_get_int(&config, "llm/llamacpp/n_threads", &llama_int) && llama_int > 0)
          refine.n_threads = llama_int;
        if (!m_refiner) {
          m_refiner = std::make_unique<PersonalRefiner>(m_personal.get());
          // 整理選字記憶時要匯出／匯入使用者詞典：在服務端的鎖下關掉所有 session 放開詞典，
          // 客戶端之後打字時發現 session 不在會自動重建
          m_refiner->SetUserDictAccess([this](const std::function<void()>& fn) {
            // 不能一直等鎖：重新載入設定時服務端會持鎖等精煉結束，一直等就互相卡死
            std::unique_lock<std::mutex> lock(weasel::ServerApiMutex(), std::defer_lock);
            for (int i = 0; i < 100 && !lock.try_lock(); ++i)
              Sleep(50);
            if (!lock.owns_lock() || m_disabled)
              return false;
            m_session_status_map.clear();
            rime_api->cleanup_all_sessions();
            fn();
            return true;
          });
          m_refiner->Configure(refine);
          m_refiner->Start();
        } else {
          m_refiner->Configure(refine);
          m_refiner->WriteStatus();
        }
      }
    }
    llm_int = 0;
    m_llm_context_idle_minutes =
        rime_api->config_get_int(&config, "llm/context/idle_minutes", &llm_int) && llm_int >= 0
            ? (unsigned)llm_int
            : 10;
    Bool llm_enabled = false;
    // 「啟用 LLM 智慧預測」是所有預測候選的總開關：關閉時個人詞庫只在背景學習，不跳出候選
    m_llm_enabled = rime_api->config_get_bool(&config, "llm/enabled", &llm_enabled) && llm_enabled;
    if (rime_api->config_get_bool(&config, "llm/enabled", &llm_enabled)) {
      if (llm_enabled) {
        // 读取 provider_type 配置项，默认为 "openai"
        const int BUF_SIZE = 64;
        char provider_type_buf[BUF_SIZE + 1] = {0};
        std::string provider_type = "openai";  // 默认值
        if (rime_api->config_get_string(&config, "llm/provider_type", provider_type_buf, BUF_SIZE)) {
          provider_type = provider_type_buf;
        }
        
        // 根据 provider_type 创建相应的 provider
        if (provider_type == "llamacpp") {
          m_llm_provider = std::make_unique<LlamaCppProvider>();
          LOG(INFO) << "LLM Provider type: llamacpp";
        } else if (provider_type == "hf_constraint") {
          m_llm_provider = std::make_unique<HFConstraintProvider>();
          LOG(INFO) << "LLM Provider type: hf_constraint";
        } else {
          // 默认使用 OpenAICompatibleProvider
          m_llm_provider = std::make_unique<OpenAICompatibleProvider>();
          LOG(INFO) << "LLM Provider type: " << provider_type << " (defaulting to openai)";
        }
        
        if (m_llm_provider->LoadConfig("weasel")) {
          LOG(INFO) << "LLM Provider initialized successfully: "
                    << m_llm_provider->GetProviderName();
          // 记下目前载入的模型，供设定画面显示
          char model_buf[1024] = {0};
          char url_buf[1024] = {0};
          if (provider_type == "llamacpp" &&
              rime_api->config_get_string(&config, "llm/llamacpp/model_path", model_buf,
                                          sizeof(model_buf) - 1)) {
            m_llm_loaded_model = u8tow(model_buf);
          } else if (provider_type == "openai") {
            // 顯示為「OpenAI 相容 API：模型（網址）」
            rime_api->config_get_string(&config, "llm/openai/model", model_buf,
                                        sizeof(model_buf) - 1);
            rime_api->config_get_string(&config, "llm/openai/api_url", url_buf,
                                        sizeof(url_buf) - 1);
            m_llm_loaded_model = L"OpenAI 相容 API：" + u8tow(model_buf) + L"（" +
                                 u8tow(url_buf) + L"）";
          } else {
            m_llm_loaded_model = u8tow(m_llm_provider->GetProviderName());
          }
        } else {
          LOG(ERROR) << "LLM Provider initialization failed: LoadConfig returned false";
          LOG(ERROR) << "Please check your weasel.yaml configuration:";
          if (provider_type == "llamacpp") {
            LOG(ERROR) << "  llm:";
            LOG(ERROR) << "    enabled: true";
            LOG(ERROR) << "    provider_type: llamacpp";
            LOG(ERROR) << "    llamacpp:";
            LOG(ERROR) << "      model_path: \"path/to/model.gguf\"";
          } else if (provider_type == "hf_constraint") {
            LOG(ERROR) << "  llm:";
            LOG(ERROR) << "    enabled: true";
            LOG(ERROR) << "    provider_type: hf_constraint";
            LOG(ERROR) << "    hf_constraint:";
            LOG(ERROR) << "      api_url: \"http://localhost:8000/v1/generate/completions\"";
          } else {
            LOG(ERROR) << "  llm:";
            LOG(ERROR) << "    enabled: true";
            LOG(ERROR) << "    provider_type: openai";
            LOG(ERROR) << "    openai:";
            LOG(ERROR) << "      api_key: \"your-api-key\"";
          }
          m_llm_provider.reset();
        }
      } else {
        LOG(INFO) << "LLM is disabled in configuration (llm/enabled = false)";
      }
    } else {
      LOG(INFO) << "LLM configuration not found (llm/enabled not set)";
    }
    
    // 注音整句校正的模型：不受「智慧預測」開關影響
    _LoadTypoProvider(&config);
    rime_api->config_close(&config);
  }
  m_last_schema_id.clear();
}

void RimeWithWeaselHandler::_RetirePersonal() {
  if (!m_refiner && !m_personal)
    return;
  if (m_refiner)
    m_refiner->RequestStop();  // 串流請求與本機生成會盡快中斷
  _WaitRetired();
  m_retire_thread = std::thread(
      [refiner = std::move(m_refiner), personal = std::move(m_personal)]() mutable {
        refiner.reset();   // 等精煉結束
        personal.reset();  // 解構時存檔
      });
}

void RimeWithWeaselHandler::_WaitRetired() {
  if (m_retire_thread.joinable())
    m_retire_thread.join();
}

void RimeWithWeaselHandler::Finalize() {
  // 個人詞庫解構時會存檔（服務結束或重新部署前）；Initialize 會重新載入
  _RetirePersonal();
  m_active_session = 0;
  m_disabled = true;
  m_session_status_map.clear();
  LOG(INFO) << "Finalizing la rime.";
  rime_api->finalize();
}

DWORD RimeWithWeaselHandler::FindSession(WeaselSessionId ipc_id) {
  if (m_disabled)
    return 0;
  Bool found = rime_api->find_session(to_session_id(ipc_id));
  DLOG(INFO) << "Find session: session_id = " << to_session_id(ipc_id)
             << ", found = " << found;
  return found ? (ipc_id) : 0;
}

DWORD RimeWithWeaselHandler::AddSession(LPWSTR buffer, EatLine eat) {
  if (m_disabled) {
    DLOG(INFO) << "Trying to resume service.";
    EndMaintenance();
    if (m_disabled)
      return 0;
  }
  RimeSessionId session_id = (RimeSessionId)rime_api->create_session();
  if (m_global_ascii_mode) {
    for (const auto& pair : m_session_status_map) {
      if (pair.first) {
        rime_api->set_option(session_id, "ascii_mode",
                             !!pair.second.status.is_ascii_mode);
        break;
      }
    }
  }

  WeaselSessionId ipc_id =
      _GenerateNewWeaselSessionId(m_session_status_map, m_pid);
  DLOG(INFO) << "Add session: created session_id = " << session_id
             << ", ipc_id = " << ipc_id;
  SessionStatus& session_status = new_session_status(ipc_id);
  session_status.style = m_base_style;
  session_status.session_id = session_id;
  _ReadClientInfo(ipc_id, buffer);

  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = status.schema_id;
    m_last_schema_id = schema_id;
    _LoadSchemaSpecificSettings(ipc_id, schema_id);
    _LoadAppInlinePreeditSet(ipc_id, true);
    _UpdateInlinePreeditStatus(ipc_id);
    _RefreshTrayIcon(session_id, _UpdateUICallback);
    session_status.status = status;
    session_status.__synced = false;
    rime_api->free_status(&status);
  }
  m_ui->style() = session_status.style;
  // show session's welcome message :-) if any
  if (eat) {
    _Respond(ipc_id, eat);
  }
  add_session = true;
  _UpdateUI(ipc_id);
  add_session = false;
  m_active_session = ipc_id;
  return ipc_id;
}

DWORD RimeWithWeaselHandler::RemoveSession(WeaselSessionId ipc_id) {
  if (m_ui)
    m_ui->Hide();
  if (m_disabled)
    return 0;
  DLOG(INFO) << "Remove session: session_id = " << to_session_id(ipc_id);
  // TODO: force committing? otherwise current composition would be lost
  rime_api->destroy_session(to_session_id(ipc_id));
  m_session_status_map.erase(ipc_id);
  m_active_session = 0;
  return 0;
}

void RimeWithWeaselHandler::UpdateColorTheme(BOOL darkMode) {
  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      m_current_dark_mode = darkMode;
      if (darkMode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
    }
    rime_api->config_close(&config);
  }

  for (auto& pair : m_session_status_map) {
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(to_session_id(pair.first), &status)) {
      _LoadSchemaSpecificSettings(pair.first, std::string(status.schema_id));
      _LoadAppInlinePreeditSet(pair.first, true);
      _UpdateInlinePreeditStatus(pair.first);
      pair.second.status = status;
      pair.second.__synced = false;
      rime_api->free_status(&status);
    }
  }
  m_ui->style() = get_session_status(m_active_session).style;
}

static ZhuyinSpeller _LoadZhuyinSpeller(RimeApi* api, const char* schema_id);

BOOL RimeWithWeaselHandler::ProcessKeyEvent(KeyEvent keyEvent,
                                            WeaselSessionId ipc_id,
                                            EatLine eat) {
  DLOG(INFO) << "Process key event: keycode = " << keyEvent.keycode
             << ", mask = " << keyEvent.mask << ", ipc_id = " << ipc_id;
  if (m_disabled)
    return FALSE;

  RimeSessionId session_id = to_session_id(ipc_id);
  // 依目前前景窗口切换上下文（之后的提交记录与预测都用该窗口自己的前文）
  if (!(keyEvent.mask & ibus::Modifier::RELEASE_MASK))
    _UpdateContextKey(ipc_id);

  // 中英混打（Shift）
  if (_HandleMixedInput(keyEvent, ipc_id, eat))
    return TRUE;
  if (_HandleZhuyinFocus(keyEvent, ipc_id, eat))
    return TRUE;

  // 处理·键（反引号键）：触发LLM预测（仅在composing状态下）或清空上下文（双击）
  if (!(keyEvent.mask & ibus::Modifier::RELEASE_MASK) &&
      (keyEvent.keycode == ibus::Keycode::grave || keyEvent.keycode == 0x060)) {
    DWORD current_time = GetTickCount();
    bool is_double_click = false;
    
    // 检测双击（500ms内连续按下两次）
    if (m_last_grave_key_time > 0 && 
        (current_time - m_last_grave_key_time) < GRAVE_DOUBLE_CLICK_TIMEOUT) {
      is_double_click = true;
    }
    m_last_grave_key_time = current_time;
    
    if (m_dev_console && m_dev_console->IsEnabled()) {
      if (is_double_click) {
        m_dev_console->WriteLine(L"[LLM] 检测到双击·键");
      } else {
        m_dev_console->WriteLine(L"[LLM] 用户按下·键");
      }
    }
    
    // 双击·键：清空上下文历史记录
    if (is_double_click) {
      if (m_context_history) {
        if (m_dev_console && m_dev_console->IsEnabled()) {
          size_t size_before = m_context_history->GetSize();
          m_dev_console->WriteLine(L"[LLM] 双击·键，清空上下文历史记录（清空前记录数: " + std::to_wstring(size_before) + L"）");
        }
        m_context_history->Clear(m_dev_console);
        if (m_dev_console && m_dev_console->IsEnabled()) {
          m_dev_console->WriteLine(L"[LLM] 上下文历史记录已清空");
        }
      } else {
        if (m_dev_console && m_dev_console->IsEnabled()) {
          m_dev_console->WriteLine(L"[LLM] 上下文历史记录未初始化，无法清空");
        }
      }
      // 清空上下文后，阻止按键继续传递
      return TRUE;
    }
    
    // 检查是否处于composing状态
    RIME_STRUCT(RimeStatus, status);
    bool is_composing = false;
    if (rime_api->get_status(session_id, &status)) {
      is_composing = !!status.is_composing;
      rime_api->free_status(&status);
    }
    
    // 只有在composing状态下才触发LLM预测，否则让·键正常输入
    if (!is_composing) {
      if (m_dev_console && m_dev_console->IsEnabled()) {
        m_dev_console->WriteLine(L"[LLM] 不在composing状态，允许·键正常输入");
      }
      // 不阻止按键，让Rime正常处理（允许输入·符号）
      // 继续执行后续代码，让Rime正常处理该按键
    } else {
      // 在composing状态下，检查LLM是否可用
      if (!m_llm_provider) {
        if (m_dev_console && m_dev_console->IsEnabled()) {
          m_dev_console->WriteLine(L"[LLM] LLM提供者未初始化");
          m_dev_console->WriteLine(L"[LLM] 请检查weasel.yaml配置文件中是否启用了LLM功能：");
          m_dev_console->WriteLine(L"[LLM]   llm:");
          m_dev_console->WriteLine(L"[LLM]     enabled: true");
          m_dev_console->WriteLine(L"[LLM]     openai:");
          m_dev_console->WriteLine(L"[LLM]       api_key: \"your-api-key\"");
        }
        // 不阻止按键，让Rime正常处理
        // 继续执行后续代码
      } else if (!m_llm_provider->IsAvailable()) {
        if (m_dev_console && m_dev_console->IsEnabled()) {
          m_dev_console->WriteLine(L"[LLM] LLM提供者已初始化，但不可用");
          m_dev_console->WriteLine(L"[LLM] 可能的原因：");
          m_dev_console->WriteLine(L"[LLM]   1. llm/enabled 未设置为 true");
          m_dev_console->WriteLine(L"[LLM]   2. llm/openai/api_key 未配置或为空");
          m_dev_console->WriteLine(L"[LLM]   3. llm/openai/api_url 未配置或为空");
        }
        // 不阻止按键，让Rime正常处理
        // 继续执行后续代码
      } else {
        // LLM可用，在composing状态下触发预测
        if (m_dev_console && m_dev_console->IsEnabled()) {
          m_dev_console->WriteLine(L"[LLM] composing状态=true，触发LLM预测");
        }
        
        // 获取当前键入的拼音（preedit）
        std::wstring current_preedit;
        RIME_STRUCT(RimeContext, ctx);
        if (rime_api->get_context(session_id, &ctx)) {
          if (ctx.composition.length > 0 && ctx.composition.preedit) {
            current_preedit = u8tow(ctx.composition.preedit);
            if (m_dev_console && m_dev_console->IsEnabled()) {
              m_dev_console->WriteLine(L"[LLM] 获取到当前拼音: " + current_preedit);
            }
          }
          rime_api->free_context(&ctx);
        }
        
        // 如果不在LLM预测模式，进入LLM预测模式（上下文统一从 m_context_history 获取）
        if (!m_llm_prediction_mode) {
          m_llm_prediction_mode = true;
          if (m_dev_console && m_dev_console->IsEnabled()) {
            if (m_context_history && m_context_history->GetSize() > 0) {
              m_dev_console->WriteLine(L"[LLM] 进入LLM预测模式，将使用上下文历史");
            } else {
              m_dev_console->WriteLine(L"[LLM] 进入LLM预测模式，上下文历史为空");
            }
          }
        }
        
        // 立即以 Rime 当前的转换结果做补全预测（注音的 preedit 是注音符号，不适合直接给模型）
        _ScheduleLLMCompletion(ipc_id, 0);
        
        // 更新UI
        // _UpdateUI(ipc_id);
        
        // 阻止按键继续传递
        return TRUE;
      }
    }
  }
  
  // 如果处于LLM预测模式，处理特殊按键
  if (m_llm_prediction_mode && !(keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    // ESC键：退出LLM预测模式；正在组字时继续交给 Rime 清除组字，一次关闭候选栏
    if (keyEvent.keycode == ibus::Keycode::Escape) {
      _ExitLLMPredictionMode(ipc_id);
      RIME_STRUCT(RimeStatus, esc_status);
      bool esc_composing = false;
      if (rime_api->get_status(session_id, &esc_status)) {
        esc_composing = !!esc_status.is_composing;
        rime_api->free_status(&esc_status);
      }
      if (!esc_composing)
        return TRUE;
    }
    
    // Tab 选第一个 LLM 候选，Shift+1~5 选第几个。
    // （原本用空格与数字键 1-9，但在注音大千键盘中这些键是注音符号与声调，会误选）
    int llm_pick = -1;
    const bool other_mods = (keyEvent.mask & (ibus::Modifier::CONTROL_MASK |
                                              ibus::Modifier::MOD1_MASK |
                                              ibus::Modifier::SUPER_MASK)) != 0;
    if (!other_mods) {
      if (keyEvent.keycode == ibus::Keycode::Tab &&
          !(keyEvent.mask & ibus::Modifier::SHIFT_MASK)) {
        llm_pick = 0;
      } else if (keyEvent.mask & ibus::Modifier::SHIFT_MASK) {
        static const UINT kShiftedDigits[] = {'!', '@', '#', '$', '%'};
        for (int k = 0; k < 5; ++k) {
          if (keyEvent.keycode == kShiftedDigits[k] || keyEvent.keycode == (UINT)('1' + k)) {
            llm_pick = k;
            break;
          }
        }
      }
    }
    if (llm_pick >= 0 && _CommitLLMCandidate(ipc_id, (size_t)llm_pick, eat)) {
      return TRUE;
    }
    
    // 如果输入的是拼音（字母），退出LLM预测模式，回到正常输入
    // （输入中补全模式下字母是注音键，由 process_key 之后的补全逻辑接手，不在这里退出以免候选栏闪烁）
    if (!m_llm_completion_active &&
        ((keyEvent.keycode >= 'a' && 
          keyEvent.keycode <= 'z') ||
         (keyEvent.keycode >= 'A' && 
          keyEvent.keycode <= 'Z'))) {
      _ExitLLMPredictionMode(ipc_id);
      // 继续处理按键，进入正常输入流程
    }
  }
  
  // 组字中打标点：Rime 会把组字连同标点直接送出；改为留在组字区（像新注音），等 Enter 一起送出
  bool punct_in_composition = false;
  if (!(keyEvent.mask & (ibus::Modifier::RELEASE_MASK | ibus::Modifier::CONTROL_MASK |
                         ibus::Modifier::MOD1_MASK | ibus::Modifier::SUPER_MASK)) &&
      keyEvent.keycode > 0x20 && keyEvent.keycode <= 0x7e &&
      !isalnum((int)keyEvent.keycode)) {
    RIME_STRUCT(RimeStatus, punct_status);
    if (rime_api->get_status(session_id, &punct_status)) {
      if (punct_status.is_composing && !punct_status.is_ascii_mode) {
        char schema_id[256] = {0};
        rime_api->get_current_schema(session_id, schema_id, sizeof(schema_id));
        punct_in_composition = _LoadZhuyinSpeller(rime_api, schema_id)
                                   .alphabet.find((char)keyEvent.keycode) == std::string::npos;
      }
      rime_api->free_status(&punct_status);
    }
  }

  // 注音组字中按 Backspace：光标前的音节已打声调（成字）就整个字删掉，还在拼的才一次删一键（像新注音）
  size_t backspaces = 1;
  if (keyEvent.keycode == ibus::Keycode::BackSpace &&
      !(keyEvent.mask & (ibus::Modifier::RELEASE_MASK | ibus::Modifier::CONTROL_MASK |
                         ibus::Modifier::MOD1_MASK | ibus::Modifier::SUPER_MASK |
                         ibus::Modifier::SHIFT_MASK))) {
    RIME_STRUCT(RimeStatus, bs_status);
    if (rime_api->get_status(session_id, &bs_status)) {
      const bool zhuyin = bs_status.is_composing && !bs_status.is_ascii_mode &&
                          bs_status.schema_id &&
                          strncmp(bs_status.schema_id, "bopomofo", 8) == 0;
      // 音节依 Rime 组字的切法（注音之间以空白分开，省略声调的连打也切得对），
      // 取光标前最后一个音节；每个注音符号或声调对应一个按键
      RIME_STRUCT(RimeContext, bs_ctx);
      if (zhuyin && rime_api->get_context(session_id, &bs_ctx)) {
        if (bs_ctx.composition.preedit && bs_ctx.composition.cursor_pos > 0) {
          const std::wstring before = u8tow(std::string(bs_ctx.composition.preedit)
                                                .substr(0, bs_ctx.composition.cursor_pos));
          size_t start = before.size();
          while (start > 0 && (zhuyin_preview::IsBopomofo(before[start - 1]) ||
                               zhuyin_preview::IsTone(before[start - 1])))
            --start;
          const size_t len = before.size() - start;
          if (len > 1 && zhuyin_preview::IsTone(before.back()))
            backspaces = len;
        }
        rime_api->free_context(&bs_ctx);
      }
      rime_api->free_status(&bs_status);
    }
  }

  Bool handled = rime_api->process_key(session_id, keyEvent.keycode,
                                       expand_ibus_modifier(keyEvent.mask));
  for (size_t i = 1; i < backspaces; ++i)
    rime_api->process_key(session_id, keyEvent.keycode, expand_ibus_modifier(keyEvent.mask));
  if (punct_in_composition) {
    RIME_STRUCT(RimeCommit, punct_commit);
    if (rime_api->get_commit(session_id, &punct_commit)) {
      if (punct_commit.text)
        get_session_status(ipc_id).mixed_text += u8tow(punct_commit.text);
      rime_api->free_commit(&punct_commit);
    }
  }
  // 混打中 Rime 不處理的可見字元（例如組字空了之後的空白）也收進混打內容，不直接輸出
  {
    SessionStatus& mixed_status = get_session_status(ipc_id);
    if (!handled && mixed_status.mixed_active() &&
        !(keyEvent.mask & (ibus::Modifier::RELEASE_MASK | ibus::Modifier::CONTROL_MASK |
                           ibus::Modifier::MOD1_MASK | ibus::Modifier::SUPER_MASK)) &&
        keyEvent.keycode >= 0x20 && keyEvent.keycode <= 0x7e) {
      mixed_status.mixed_text += (wchar_t)keyEvent.keycode;
      handled = True;
    }
  }
  // 输入中补全：正在组字时，停顿 300ms 后以 Rime 当前转换结果续写；组字结束则清除补全候选
  if (handled && !(keyEvent.mask & ibus::Modifier::RELEASE_MASK) &&
      (_PredictionAvailable() || _TypoLLMAvailable())) {
    bool composing = false;
    RIME_STRUCT(RimeStatus, st);
    if (rime_api->get_status(session_id, &st)) {
      composing = st.is_composing && !st.is_ascii_mode;
      rime_api->free_status(&st);
    }
    if (composing && ((m_llm_while_typing && _PredictionAvailable()) || _TypoLLMAvailable())) {
      _ScheduleLLMCompletion(ipc_id, kLLMCompletionDelayMs);
    } else if (composing && m_llm_prediction_mode) {
      // 未开启输入中补全：开始打字后，提交后留下的下一词预测已不适用，直接清掉
      m_llm_prediction_mode = false;
      std::lock_guard<std::mutex> lock(m_llm_mutex);
      ++m_llm_request_seq;
      m_current_llm_candidates.clear();
    } else if (!composing) {
      _CancelLLMCompletion();
    }
  }
  // vim_mode when keydown only
  if (!handled && !(keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    bool isVimBackInCommandMode =
        (keyEvent.keycode == ibus::Keycode::Escape) ||
        ((keyEvent.mask & (1 << 2)) &&
         (keyEvent.keycode == ibus::Keycode::XK_c ||
          keyEvent.keycode == ibus::Keycode::XK_C ||
          keyEvent.keycode == ibus::Keycode::XK_bracketleft));
    if (isVimBackInCommandMode &&
        rime_api->get_option(session_id, "vim_mode") &&
        !rime_api->get_option(session_id, "ascii_mode")) {
      rime_api->set_option(session_id, "ascii_mode", True);
    }
  }
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
  return (BOOL)handled;
}

std::wstring RimeWithWeaselHandler::_TakeComposition(RimeSessionId session_id) {
  std::wstring text;
  rime_api->commit_composition(session_id);
  RIME_STRUCT(RimeCommit, commit);
  if (rime_api->get_commit(session_id, &commit)) {
    if (commit.text)
      text = u8tow(commit.text);
    rime_api->free_commit(&commit);
  }
  return text;
}

bool RimeWithWeaselHandler::_HandleMixedInput(const weasel::KeyEvent& keyEvent,
                                              WeaselSessionId ipc_id,
                                              EatLine eat) {
  SessionStatus& ss = get_session_status(ipc_id);
  const RimeSessionId session_id = ss.session_id;
  const UINT key = keyEvent.keycode;
  const bool release = (keyEvent.mask & ibus::Modifier::RELEASE_MASK) != 0;
  const bool is_shift = key == ibus::Keycode::Shift_L || key == ibus::Keycode::Shift_R;
  const bool other_mods = (keyEvent.mask & (ibus::Modifier::CONTROL_MASK |
                                            ibus::Modifier::MOD1_MASK |
                                            ibus::Modifier::SUPER_MASK)) != 0;
  bool composing = false;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    composing = !!status.is_composing;
    rime_api->free_status(&status);
  }
  // 吃掉按鍵時一定要回應完整的目前狀態：TSF 讀到空的回應會當成組字結束，把組字清掉
  auto respond = [&]() {
    _Respond(ipc_id, eat);
    _UpdateUI(ipc_id);
    m_active_session = ipc_id;
    return true;
  };
  // 送出混打內容與目前的組字，回到一般輸入
  auto commit_all = [&]() {
    std::wstring text = ss.mixed_text;
    if (composing)
      text += _TakeComposition(session_id);
    ss.mixed_text.clear();
    ss.mixed_english = false;
    ss.mixed_commit += text;
    if (m_llm_prediction_mode || m_llm_completion_active)
      _ExitLLMPredictionMode(ipc_id);
  };

  // Shift（左右皆可）單獨按下再放開：切換中英。組字中或混打中才由這裡處理，其餘交給 Rime 照舊切換
  if (!release)
    m_mixed_shift_tap = is_shift && !other_mods;
  if (release && is_shift && m_mixed_shift_tap) {
    m_mixed_shift_tap = false;
    if (!composing && !ss.mixed_active())
      return false;
    // 英文段不切換 Rime 的 ascii_mode：英文字母都由這裡處理；切成英數會讓應用程式
    // 看到輸入模式改變，有些程式因此把組字藏起來，直到切回中文
    if (ss.mixed_english) {
      ss.mixed_english = false;
    } else {
      if (composing) {
        ss.mixed_text += _TakeComposition(session_id);
        if (m_llm_prediction_mode || m_llm_completion_active)
          _ExitLLMPredictionMode(ipc_id);
      }
      ss.mixed_english = true;
    }
    // Rime 只收到這次 Shift 的按下：送一個無作用的鍵，重設它記住的 Shift 狀態
    rime_api->process_key(session_id, 0xffffff /* VoidSymbol */, 0);
    return respond();
  }
  if (!ss.mixed_active())
    return false;
  // 混打中的 Shift 不交給 Rime，免得 Shift+字母（大寫）之後 Rime 自己切換中英
  if (is_shift)
    return respond();
  const bool editing_key = key == ibus::Keycode::Return || key == ibus::Keycode::KP_Enter ||
                           key == ibus::Keycode::BackSpace || key == ibus::Keycode::Escape;
  const bool printable = key >= 0x20 && key <= 0x7e && !other_mods;
  if (release)  // 按下時由這裡處理的鍵，放開也吃掉
    return ss.mixed_english && (printable || editing_key) && respond();

  if (key == ibus::Keycode::Return || key == ibus::Keycode::KP_Enter) {
    commit_all();
    return respond();
  }
  if (key == ibus::Keycode::Escape) {
    rime_api->clear_composition(session_id);
    ss.mixed_text.clear();
    ss.mixed_english = false;
    if (m_llm_prediction_mode || m_llm_completion_active)
      _ExitLLMPredictionMode(ipc_id);
    return respond();
  }
  if (ss.mixed_english || !composing) {
    if (key == ibus::Keycode::BackSpace) {
      if (!ss.mixed_text.empty()) {
        ss.mixed_text.pop_back();
        if (!ss.mixed_text.empty() && IS_HIGH_SURROGATE(ss.mixed_text.back()))
          ss.mixed_text.pop_back();
      }
      // 刪光了：結束混打，停在目前的中英模式
      if (ss.mixed_text.empty())
        ss.mixed_english = false;
      return respond();
    }
    if (ss.mixed_english && printable) {
      ss.mixed_text += (wchar_t)key;
      return respond();
    }
    // 游標移動等按鍵：先送出混打內容，按鍵再交給應用程式
    const bool leaving_key =
        key == ibus::Keycode::Left || key == ibus::Keycode::Right || key == ibus::Keycode::Up ||
        key == ibus::Keycode::Down || key == ibus::Keycode::Home || key == ibus::Keycode::End ||
        key == ibus::Keycode::Page_Up || key == ibus::Keycode::Page_Down ||
        key == ibus::Keycode::Delete ||
        (key == ibus::Keycode::Tab && m_current_llm_candidates.empty());
    if (leaving_key || (other_mods && key < 0xff00)) {
      commit_all();  // 由之後一般流程的 _Respond 送出
      return false;
    }
  }
  return false;
}

// 框住第 index 個音節：前面的字照目前顯示的样子確定下來，Rime 的候選就只針對這個字；
// 反白停在目前顯示的字。靠的是最後一次整句轉換的快取（每個音節一個字）
bool RimeWithWeaselHandler::_FocusSyllable(WeaselSessionId ipc_id, int index) {
  SessionStatus& ss = get_session_status(ipc_id);
  const RimeSessionId session_id = ss.session_id;
  const char* raw = rime_api->get_input(session_id);
  const std::string input = raw ? raw : "";
  // 每個字對應的按鍵數，依整句轉換時 Rime 的切法（省略聲調的連打也對得上）
  const auto& units = ss.preview_units;
  const std::vector<size_t> lens = ss.preview_lens;
  if (index < 0 || index >= (int)lens.size() || units.size() != lens.size() ||
      ss.preview_input != input)
    return false;
  size_t end = 0;
  for (int i = 0; i <= index; ++i)
    end += lens[i];

  // 找候選清單裡符合條件的候選序號
  auto find_candidate = [&](auto&& match) {
    int found = -1, idx = 0;
    RimeCandidateListIterator it = {0};
    if (rime_api->candidate_list_begin(session_id, &it)) {
      while (rime_api->candidate_list_next(&it) && idx < 300) {
        if (it.candidate.text && match(u8tow(it.candidate.text), idx))
          found = idx;
        ++idx;
      }
      rime_api->candidate_list_end(&it);
    }
    return found;
  };

  const std::vector<std::wstring> units_copy = units;  // set_input 之後快取不會變，保險起見複製
  rime_api->clear_composition(session_id);
  rime_api->set_input(session_id, input.c_str());
  rime_api->set_caret_pos(session_id, end);
  int pos = 0;
  while (pos < index) {
    int best_len = 0;
    const int best = find_candidate([&](const std::wstring& text, int) {
      const int len = (int)zhuyin_preview::SplitChars(text).size();
      if (len > best_len && len <= index - pos &&
          text == zhuyin_preview::Join(units_copy, pos, pos + len)) {
        best_len = len;
        return true;
      }
      return false;
    });
    if (best < 0 || !rime_api->select_candidate(session_id, best)) {
      // 對不上：把整句恢復原狀
      rime_api->clear_composition(session_id);
      rime_api->set_input(session_id, input.c_str());
      ss.focus = -1;
      return false;
    }
    pos += best_len;
  }
  bool first = true;
  const int shown = find_candidate([&](const std::wstring& text, int) {
    const bool hit = first && text == units_copy[index];
    if (hit)
      first = false;
    return hit;
  });
  if (shown > 0)
    rime_api->highlight_candidate(session_id, shown);
  ss.focus = index;
  ss.focus_input = input;
  ss.focus_caret = end;
  return true;
}

bool RimeWithWeaselHandler::_HandleZhuyinFocus(const weasel::KeyEvent& keyEvent,
                                               WeaselSessionId ipc_id,
                                               EatLine eat) {
  const UINT key = keyEvent.keycode;
  const bool nav = key == ibus::Keycode::Left || key == ibus::Keycode::Right ||
                   key == ibus::Keycode::Home || key == ibus::Keycode::End;
  if (!nav || (keyEvent.mask & (ibus::Modifier::CONTROL_MASK | ibus::Modifier::MOD1_MASK |
                                ibus::Modifier::SUPER_MASK | ibus::Modifier::SHIFT_MASK)))
    return false;
  SessionStatus& ss = get_session_status(ipc_id);
  const RimeSessionId session_id = ss.session_id;
  RIME_STRUCT(RimeStatus, status);
  bool zhuyin = false;
  if (rime_api->get_status(session_id, &status)) {
    zhuyin = status.is_composing && !status.is_ascii_mode && status.schema_id &&
             strncmp(status.schema_id, "bopomofo", 8) == 0;
    rime_api->free_status(&status);
  }
  if (!zhuyin)
    return false;
  auto respond = [&]() {
    _Respond(ipc_id, eat);
    _UpdateUI(ipc_id);
    m_active_session = ipc_id;
    return true;
  };
  if (keyEvent.mask & ibus::Modifier::RELEASE_MASK)
    return ss.focus >= 0 && respond();

  const char* raw = rime_api->get_input(session_id);
  const std::string input = raw ? raw : "";
  // 字數依整句轉換時記下的切法；對不上（例如句中插入過字）就交回 Rime 原本的游標移動
  if (ss.preview_input != input || ss.preview_lens.empty())
    return false;
  const int count = (int)ss.preview_lens.size();
  // 回到句尾：取消框選，接著打字
  auto unfocus = [&]() {
    rime_api->set_caret_pos(session_id, input.size());
    ss.focus = -1;
    return respond();
  };
  int target;
  if (key == ibus::Keycode::Left) {
    target = ss.focus < 0 ? count - 1 : (std::max)(0, ss.focus - 1);
  } else if (key == ibus::Keycode::Home) {
    target = 0;
  } else if (ss.focus < 0) {
    return false;  // 本來就在句尾
  } else if (key == ibus::Keycode::End || ss.focus + 1 >= count) {
    return unfocus();
  } else {
    target = ss.focus + 1;
  }
  if (!_FocusSyllable(ipc_id, target))
    return false;  // 對不上（例如省略聲調的連打），交回 Rime 原本的游標移動
  return respond();
}

void RimeWithWeaselHandler::CommitComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Commit composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  SessionStatus& ss = get_session_status(ipc_id);
  if (ss.mixed_active()) {
    // 混打内容连同组字一起，在下一次回应时送出
    ss.mixed_commit += ss.mixed_text + _TakeComposition(to_session_id(ipc_id));
    ss.mixed_text.clear();
    ss.mixed_english = false;
  } else {
    rime_api->commit_composition(to_session_id(ipc_id));
  }
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::ClearComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Clear composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  SessionStatus& ss = get_session_status(ipc_id);
  ss.mixed_text.clear();
  ss.mixed_english = false;
  rime_api->clear_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::SelectCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id) {
  DLOG(INFO) << "select candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  LOG(INFO) << "[DEBUG] SelectCandidateOnCurrentPage called: index=" << index 
            << ", ipc_id=" << ipc_id << ", llm_mode=" << m_llm_prediction_mode;
  
  if (m_disabled)
    return;

  RimeSessionId session_id = to_session_id(ipc_id);
  _UpdateContextKey(ipc_id);

  // 如果处于LLM预测模式，检查是否选择的是LLM候选词
  if (m_llm_prediction_mode && !m_current_llm_candidates.empty()) {
    RIME_STRUCT(RimeContext, ctx);
    size_t rime_candidate_count = 0;
    
    if (rime_api->get_context(session_id, &ctx)) {
      rime_candidate_count = ctx.menu.num_candidates;
      rime_api->free_context(&ctx);
    }
    
    LOG(INFO) << "[DEBUG] In LLM mode: rime_count=" << rime_candidate_count 
              << ", llm_count=" << m_current_llm_candidates.size();
    
    // 如果索引超出或等于Rime候选词范围，说明选择的是LLM候选词
    if (index >= rime_candidate_count) {
      LOG(INFO) << "[LLM] Selected LLM candidate: " << index - rime_candidate_count + 1;
      if (_CommitLLMCandidate(ipc_id, index - rime_candidate_count, nullptr))
        return;
    }
  }

  // 如果不是LLM候选词或不在LLM模式，按照正常流程处理Rime候选词
  LOG(INFO) << "[DEBUG] Processing as Rime candidate";
  _CancelLLMCompletion();
  rime_api->select_candidate_on_current_page(session_id, index);
}

bool RimeWithWeaselHandler::HighlightCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id,
    EatLine eat) {
  DLOG(INFO) << "highlight candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  bool res = rime_api->highlight_candidate_on_current_page(
      to_session_id(ipc_id), index);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

bool RimeWithWeaselHandler::ChangePage(bool backward,
                                       WeaselSessionId ipc_id,
                                       EatLine eat) {
  DLOG(INFO) << "change page, ipc_id = " << ipc_id
             << (backward ? "backward" : "foreward");
  bool res = rime_api->change_page(to_session_id(ipc_id), backward);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

void RimeWithWeaselHandler::FocusIn(DWORD client_caps, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus in: ipc_id = " << ipc_id
             << ", client_caps = " << client_caps;
  if (m_disabled)
    return;
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::FocusOut(DWORD param, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus out: ipc_id = " << ipc_id;
  
  // 退出LLM预测模式（如果处于该模式）
  if (m_llm_prediction_mode) {
    _ExitLLMPredictionMode(ipc_id);
  }
  
  if (m_ui)
    m_ui->Hide();
  m_active_session = 0;
}

void RimeWithWeaselHandler::UpdateInputPosition(RECT const& rc,
                                                WeaselSessionId ipc_id) {
  DLOG(INFO) << "Update input position: (" << rc.left << ", " << rc.top
             << "), ipc_id = " << ipc_id
             << ", m_active_session = " << m_active_session;
  if (m_ui)
    m_ui->UpdateInputPosition(rc);
  if (m_disabled)
    return;
  if (m_active_session != ipc_id) {
    _UpdateUI(ipc_id);
    m_active_session = ipc_id;
  }
}

std::string RimeWithWeaselHandler::m_message_type;
std::string RimeWithWeaselHandler::m_message_value;
std::string RimeWithWeaselHandler::m_message_label;
std::string RimeWithWeaselHandler::m_option_name;

void RimeWithWeaselHandler::OnNotify(void* context_object,
                                     uintptr_t session_id,
                                     const char* message_type,
                                     const char* message_value) {
  // may be running in a thread when deploying rime
  RimeWithWeaselHandler* self =
      reinterpret_cast<RimeWithWeaselHandler*>(context_object);
  if (!self || !message_type || !message_value)
    return;
  m_message_type = message_type;
  m_message_value = message_value;
  if (RIME_API_AVAILABLE(rime_api, get_state_label) &&
      !strcmp(message_type, "option")) {
    Bool state = message_value[0] != '!';
    const char* option_name = message_value + !state;
    m_option_name = option_name;
    const char* state_label =
        rime_api->get_state_label(session_id, option_name, state);
    if (state_label) {
      m_message_label = std::string(state_label);
    }
  }
}

void RimeWithWeaselHandler::_ReadClientInfo(WeaselSessionId ipc_id,
                                            LPWSTR buffer) {
  std::string app_name;
  std::string client_type;
  // parse request text
  WMemStream bs((wchar_t*)buffer, WEASEL_IPC_BUFFER_LENGTH);
  std::wstring line;
  while (bs.good()) {
    std::getline(bs, line);
    if (!bs.good())
      break;
    // file ends
    if (line == L".")
      break;
    const std::wstring kClientAppKey = L"session.client_app=";
    if (starts_with(line, kClientAppKey)) {
      std::wstring lwr = line;
      to_lower(lwr);
      app_name = wtou8(lwr.substr(kClientAppKey.length()));
    }
    const std::wstring kClientTypeKey = L"session.client_type=";
    if (starts_with(line, kClientTypeKey)) {
      client_type = wtou8(line.substr(kClientTypeKey.length()));
    }
  }
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set app specific options
  if (!app_name.empty()) {
    rime_api->set_property(session_id, "client_app", app_name.c_str());

    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        DLOG(INFO) << "set app option: " << pair.first << " = " << pair.second;
        rime_api->set_option(session_id, pair.first.c_str(), Bool(pair.second));
      }
    }
  }
  // ime | tsf
  rime_api->set_property(session_id, "client_type", client_type.c_str());
  // inline preedit
  bool inline_preedit =
      session_status.style.inline_preedit && (client_type == "tsf");
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}

void RimeWithWeaselHandler::_GetCandidateInfo(CandidateInfo& cinfo,
                                              RimeContext& ctx) {
  // 先保存LLM预测模式状态，因为后面可能会被修改
  bool llm_mode = m_llm_prediction_mode;
  // 后台预测线程会替换 m_current_llm_candidates，这里在锁内取快照再使用
  std::vector<std::wstring> llm_candidates;
  size_t correction_count = 0;
  {
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    llm_candidates = m_current_llm_candidates;
    correction_count = m_llm_correction_count;
  }
  size_t llm_candidate_count = llm_candidates.size();

  // 先清空候选词信息，避免重复添加
  cinfo.candies.clear();
  cinfo.comments.clear();
  cinfo.labels.clear();
  
  // 重新设置大小以容纳Rime候选词
  cinfo.candies.resize(ctx.menu.num_candidates);
  cinfo.comments.resize(ctx.menu.num_candidates);
  cinfo.labels.resize(ctx.menu.num_candidates);
  
  // 处理Rime候选词
  for (int i = 0; i < ctx.menu.num_candidates; ++i) {
    cinfo.candies[i].str = escape_string(u8tow(ctx.menu.candidates[i].text));
    if (ctx.menu.candidates[i].comment) {
      cinfo.comments[i].str =
          escape_string(u8tow(ctx.menu.candidates[i].comment));
    }
    if (RIME_STRUCT_HAS_MEMBER(ctx, ctx.select_labels) && ctx.select_labels) {
      cinfo.labels[i].str = escape_string(u8tow(ctx.select_labels[i]));
    } else if (ctx.menu.select_keys) {
      cinfo.labels[i].str =
          escape_string(std::wstring(1, ctx.menu.select_keys[i]));
    } else {
      cinfo.labels[i].str = std::to_wstring((i + 1) % 10);
    }
  }
  cinfo.highlighted = ctx.menu.highlighted_candidate_index;
  cinfo.currentPage = ctx.menu.page_no;
  cinfo.is_last_page = ctx.menu.is_last_page;
  
  // 如果处于LLM预测模式，添加LLM候选词
  if (llm_mode && llm_candidate_count > 0) {
    size_t rime_count = cinfo.candies.size();
    
    if (m_dev_console && m_dev_console->IsEnabled()) {
      std::wstringstream ss;
      ss << L"[DEBUG] _GetCandidateInfo: Rime候选词数=" << rime_count 
         << L", LLM候选词数=" << llm_candidate_count;
      m_dev_console->WriteLine(ss.str());
    }
    
    // 将LLM候选词追加到Rime候选词后面
    for (size_t i = 0; i < llm_candidate_count; ++i) {
      Text llm_text;
      // 与 Rime 候选词一致必须 escape：IPC 按行传输，未转义的换行会截断消息，
      // 导致客户端（加载在各应用进程内的 TSF）反序列化抛异常而使宿主应用崩溃
      llm_text.str = escape_string(llm_candidates[i]);
      cinfo.candies.push_back(llm_text);
      
      // 标签显示对应按键：Tab 选第一个，Shift+2~5 选其余（数字键在注音中是注音符号）
      Text label;
      label.str = (i == 0) ? std::wstring(L"Tab") : L"⇧" + std::to_wstring(i + 1);
      cinfo.labels.push_back(label);
      
      // 整句校正的候选标示「校正」，其余为空注释
      Text comment;
      comment.str = i < correction_count ? L"校正" : L"";
      cinfo.comments.push_back(comment);
      
      if (m_dev_console && m_dev_console->IsEnabled()) {
        std::wstringstream ss;
        // ss << L"[DEBUG] 添加LLM候选词到UI: " << label_index << L". " << llm_text.str;
        m_dev_console->WriteLine(ss.str());
      }
    }
    
    if (m_dev_console && m_dev_console->IsEnabled()) {
      std::wstringstream ss;
      ss << L"[DEBUG] _GetCandidateInfo: 添加LLM候选词后，总候选词数=" << cinfo.candies.size();
      m_dev_console->WriteLine(ss.str());
    }
  }
}

void RimeWithWeaselHandler::StartMaintenance() {
  m_session_status_map.clear();
  Finalize();
  _UpdateUI(0);
}

void RimeWithWeaselHandler::EndMaintenance() {
  if (m_disabled) {
    Initialize();
    _UpdateUI(0);
  }
  m_session_status_map.clear();
}

void RimeWithWeaselHandler::SetOption(WeaselSessionId ipc_id,
                                      const std::string& opt,
                                      bool val) {
  // from no-session client, not actual typing session
  if (!ipc_id) {
    if (m_global_ascii_mode && opt == "ascii_mode") {
      for (auto& pair : m_session_status_map)
        rime_api->set_option(to_session_id(pair.first), "ascii_mode", val);
    } else {
      rime_api->set_option(to_session_id(m_active_session), opt.c_str(), val);
    }
  } else {
    rime_api->set_option(to_session_id(ipc_id), opt.c_str(), val);
  }
}

void RimeWithWeaselHandler::OnUpdateUI(std::function<void()> const& cb) {
  _UpdateUICallback = cb;
}

bool RimeWithWeaselHandler::_IsDeployerRunning() {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  bool deployer_detected = hMutex && GetLastError() == ERROR_ALREADY_EXISTS;
  if (hMutex) {
    CloseHandle(hMutex);
  }
  return deployer_detected;
}

void RimeWithWeaselHandler::_UpdateUI(WeaselSessionId ipc_id) {
  // 快速检查：如果UI对象不存在，直接返回
  if (!m_ui) {
    if (m_dev_console && m_dev_console->IsEnabled()) {
      m_dev_console->WriteLine(L"[_UpdateUI] 错误: m_ui 为 nullptr，退出");
    }
    return;
  }

  // 获取会话信息
  RimeSessionId session_id = to_session_id(ipc_id);
  bool is_tsf = _IsSessionTSF(session_id);

  // 准备状态和上下文
  Status& weasel_status = m_ui->status();
  Context weasel_context;
  
  if (ipc_id == 0) {
    weasel_status.disabled = m_disabled;
  }

  // 获取状态信息
  _GetStatus(weasel_status, ipc_id, weasel_context);

  // 判断是否需要获取上下文
  // - 非TSF模式：总是获取
  // - 有LLM候选词时：无论是否TSF，都需要获取，以便在_UI中合并Rime+LLM候选
  bool has_llm_candidates = false;


  {
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    has_llm_candidates = m_llm_prediction_mode && !m_current_llm_candidates.empty();
  }

  bool need_context = !is_tsf || has_llm_candidates;
  
  if (need_context) {
    _GetContext(weasel_context, session_id);
    // 中英混打：服务端候选窗（非 TSF）的组字前面接上混打内容
    const SessionStatus& mixed_status = get_session_status(ipc_id);
    if (mixed_status.mixed_active()) {
      const int offset = (int)mixed_status.mixed_text.size();
      weasel_context.preedit.str = mixed_status.mixed_text + weasel_context.preedit.str;
      for (auto& attr : weasel_context.preedit.attributes) {
        attr.range.start += offset;
        attr.range.end += offset;
        if (attr.range.cursor >= 0)
          attr.range.cursor += offset;
      }
    }
  }

  // 更新会话样式设置
  SessionStatus& session_status = get_session_status(ipc_id);
  if (rime_api->get_option(session_id, "inline_preedit")) {
    session_status.style.client_caps |= INLINE_PREEDIT_CAPABLE;
  } else {
    session_status.style.client_caps &= ~INLINE_PREEDIT_CAPABLE;
  }

  // 判断是否应该显示UI
  // 条件1：正在输入且非TSF模式
  // 条件2：LLM预测模式且有候选词
  bool should_show_ui = (weasel_status.composing && !is_tsf) || has_llm_candidates;
  
  if (should_show_ui) {
    // 显示UI
    m_ui->Update(weasel_context, weasel_status);
    m_ui->Show();
    // TSF 的候选窗由应用进程内的 TSF 绘制；这里弹出的是服务端候选窗（用于显示异步完成的 LLM 结果）
    if (is_tsf)
      m_llm_server_ui_shown = true;
  } else {
    // 检查是否有消息需要显示
    bool has_message = _ShowMessage(weasel_context, weasel_status);

    // 如果没有消息且非TSF模式，隐藏UI
    if (!has_message && !is_tsf) {
    m_ui->Hide();
    m_ui->Update(weasel_context, weasel_status);
    } else if (!has_message && is_tsf && m_llm_server_ui_shown) {
      // TSF 下 LLM 候选已清除（继续打字/提交/取消）：收起服务端候选窗，交回 TSF 自己的候选窗，
      // 否则旧窗会一直盖在 TSF 候选窗上面
      m_ui->Hide();
      m_llm_server_ui_shown = false;
    }
  }

  // 刷新托盘图标
  _RefreshTrayIcon(session_id, _UpdateUICallback);

  // 清空消息缓存
  m_message_type.clear();
  m_message_value.clear();
  m_message_label.clear();
  m_option_name.clear();
}


// void RimeWithWeaselHandler::_UpdateUI(WeaselSessionId ipc_id) {
//   // if m_ui nullptr, _UpdateUI meaningless
//   if (!m_ui)
//     return;

//   Status& weasel_status = m_ui->status();
//   Context weasel_context;

//   RimeSessionId session_id = to_session_id(ipc_id);
//   bool is_tsf = _IsSessionTSF(session_id);

//   if (ipc_id == 0)
//     weasel_status.disabled = m_disabled;

//   _GetStatus(weasel_status, ipc_id, weasel_context);

//   if (!is_tsf) {
//     _GetContext(weasel_context, session_id);
//   }

//   SessionStatus& session_status = get_session_status(ipc_id);
//   if (rime_api->get_option(session_id, "inline_preedit"))
//     session_status.style.client_caps |= INLINE_PREEDIT_CAPABLE;
//   else
//     session_status.style.client_caps &= ~INLINE_PREEDIT_CAPABLE;

//   if (weasel_status.composing && !is_tsf) {
//     m_ui->Update(weasel_context, weasel_status);
//     m_ui->Show();
//   } else if (!_ShowMessage(weasel_context, weasel_status) && !is_tsf) {
//     m_ui->Hide();
//     m_ui->Update(weasel_context, weasel_status);
//   }

//   _RefreshTrayIcon(session_id, _UpdateUICallback);

//   m_message_type.clear();
//   m_message_value.clear();
//   m_message_label.clear();
//   m_option_name.clear();
// }

void RimeWithWeaselHandler::_LoadSchemaSpecificSettings(
    WeaselSessionId ipc_id,
    const std::string& schema_id) {
  if (!m_ui)
    return;
  RimeConfig config;
  if (!rime_api->schema_open(schema_id.c_str(), &config))
    return;
  _UpdateShowNotifications(&config);
  m_ui->style() = m_base_style;
  _UpdateUIStyle(&config, m_ui, false);
  SessionStatus& session_status = get_session_status(ipc_id);
  session_status.style = m_ui->style();
  UIStyle& style = session_status.style;
  // load schema color style config
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  const auto update_color_scheme = [&]() {
    std::string color_name(buffer);
    RimeConfigIterator preset = {0};
    if (rime_api->config_begin_map(
            &preset, &config, ("preset_color_schemes/" + color_name).c_str())) {
      _UpdateUIStyleColor(&config, style, color_name);
      rime_api->config_end(&preset);
    } else {
      RimeConfig weaselconfig;
      if (rime_api->config_open("weasel", &weaselconfig)) {
        _UpdateUIStyleColor(&weaselconfig, style, color_name);
        rime_api->config_close(&weaselconfig);
      }
    }
  };
  const char* key =
      m_current_dark_mode ? "style/color_scheme_dark" : "style/color_scheme";
  if (rime_api->config_get_string(&config, key, buffer, BUF_SIZE))
    update_color_scheme();
  // load schema icon start
  {
    const auto load_icon = [](RimeConfig& config, const char* key1,
                              const char* key2) {
      const auto user_dir = WeaselUserDataPath();
      const auto shared_dir = WeaselSharedDataPath();
      const int BUF_SIZE = 255;
      char buffer[BUF_SIZE + 1] = {0};
      if (rime_api->config_get_string(&config, key1, buffer, BUF_SIZE) ||
          (key2 != NULL &&
           rime_api->config_get_string(&config, key2, buffer, BUF_SIZE))) {
        auto resource = u8tow(buffer);
        if (fs::is_regular_file(user_dir / resource))
          return (user_dir / resource).wstring();
        else if (fs::is_regular_file(shared_dir / resource))
          return (shared_dir / resource).wstring();
      }
      return std::wstring();
    };
    style.current_zhung_icon =
        load_icon(config, "schema/icon", "schema/zhung_icon");
    style.current_ascii_icon = load_icon(config, "schema/ascii_icon", NULL);
    style.current_full_icon = load_icon(config, "schema/full_icon", NULL);
    style.current_half_icon = load_icon(config, "schema/half_icon", NULL);
  }
  // load schema icon end
  rime_api->config_close(&config);
}

void RimeWithWeaselHandler::_LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                                     bool ignore_app_name) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  static char _app_name[50];
  rime_api->get_property(session_id, "client_app", _app_name,
                         sizeof(_app_name) - 1);
  std::string app_name(_app_name);
  if (!ignore_app_name && m_last_app_name == app_name)
    return;
  m_last_app_name = app_name;
  bool inline_preedit = session_status.style.inline_preedit;
  bool found = false;
  if (!app_name.empty()) {
    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        if (pair.first == "inline_preedit") {
          rime_api->set_option(session_id, pair.first.c_str(),
                               Bool(pair.second));
          session_status.style.inline_preedit = Bool(pair.second);
          found = true;
          break;
        }
      }
    }
  }
  if (!found) {
    session_status.style.inline_preedit = m_base_style.inline_preedit;
    // load from schema.
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(session_id, &status)) {
      std::string schema_id = status.schema_id;
      RimeConfig config;
      if (rime_api->schema_open(schema_id.c_str(), &config)) {
        Bool value = False;
        if (rime_api->config_get_bool(&config, "style/inline_preedit",
                                      &value)) {
          session_status.style.inline_preedit = value;
        }
        rime_api->config_close(&config);
      }
      rime_api->free_status(&status);
    }
  }
  if (session_status.style.inline_preedit != inline_preedit)
    _UpdateInlinePreeditStatus(ipc_id);
}

bool RimeWithWeaselHandler::_ShowMessage(Context& ctx, Status& status) {
  // show as auxiliary string
  std::wstring& tips(ctx.aux.str);
  bool show_icon = false;
  if (m_message_type == "deploy") {
    if (m_message_value == "start")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deploying RIME";
      else
        tips = L"正在部署 RIME";
    else if (m_message_value == "success")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deployed";
      else
        tips = L"部署完成";
    else if (m_message_value == "failure") {
      if (GetThreadUILanguage() ==
          MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
        tips = L"有錯誤，請查看日誌 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else if (GetThreadUILanguage() ==
               MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
        tips = L"有错误，请查看日志 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else
        tips =
            L"There is an error, please check the logs "
            L"%TEMP%\\rime.weasel\\rime.weasel.*.INFO";
    }
  } else if (m_message_type == "schema") {
    tips = /*L"【" + */ status.schema_name /* + L"】"*/;
  } else if (m_message_type == "option") {
    status.type = SCHEMA;
    if (m_message_value == "!ascii_mode") {
      show_icon = true;
    } else if (m_message_value == "ascii_mode") {
      show_icon = true;
    } else
      tips = u8tow(m_message_label);

    if (m_message_value == "full_shape" || m_message_value == "!full_shape")
      status.type = FULL_SHAPE;
  }
  if (tips.empty() && !show_icon)
    return m_ui->IsCountingDown();
  auto foption = m_show_notifications.find(m_option_name);
  auto falways = m_show_notifications.find("always");
  if ((!add_session && (foption != m_show_notifications.end() ||
                        falways != m_show_notifications.end())) ||
      m_message_type == "deploy") {
    m_ui->Update(ctx, status);
    if (m_show_notifications_time)
      m_ui->ShowWithTimeout(m_show_notifications_time);
    return true;
  } else {
    return m_ui->IsCountingDown();
  }
}
inline std::string _GetLabelText(const std::vector<Text>& labels,
                                 int id,
                                 const wchar_t* format) {
  wchar_t buffer[128];
  swprintf_s<128>(buffer, format, labels.at(id).str.c_str());
  return wtou8(std::wstring(buffer));
}

// speller settings of the schema, used to build the preview preedit
static ZhuyinSpeller _LoadZhuyinSpeller(RimeApi* api, const char* schema_id) {
  ZhuyinSpeller sp;
  RimeConfig config = {NULL};
  if (!schema_id || !api->schema_open(schema_id, &config))
    return sp;
  if (const char* finals = api->config_get_cstring(&config, "speller/finals"))
    sp.finals = finals;
  if (const char* alphabet = api->config_get_cstring(&config, "speller/alphabet"))
    sp.alphabet = alphabet;
  if (const char* delim = api->config_get_cstring(&config, "speller/delimiter"))
    if (*delim)
      sp.delimiter = *delim;
  const size_t n = api->config_list_size(&config, "translator/preedit_format");
  for (size_t i = 0; i < n; ++i) {
    const std::string key =
        "translator/preedit_format/@" + std::to_string(i);
    const char* rule = api->config_get_cstring(&config, key.c_str());
    if (!rule || strncmp(rule, "xlit", 4) != 0 || !rule[4])
      continue;
    const std::string r(rule + 5);
    const char sep = rule[4];
    const size_t mid = r.find(sep);
    if (mid == std::string::npos)
      continue;
    const size_t end = r.find(sep, mid + 1);
    const std::wstring from = u8tow(r.substr(0, mid));
    const std::wstring to = u8tow(r.substr(mid + 1, end - mid - 1));
    for (size_t k = 0; k < from.size() && k < to.size(); ++k)
      sp.xlit[from[k]] = to[k];
  }
  api->config_close(&config);
  return sp;
}

bool RimeWithWeaselHandler::_Respond(WeaselSessionId ipc_id, EatLine eat) {
  std::set<std::string> actions;
  std::list<std::string> messages;

  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  
  // 处理待提交的LLM候选词（混打中则接在混打内容后面，等 Enter 一起送出）
  if (!m_pending_llm_commit.empty()) {
    if (session_status.mixed_active()) {
      session_status.mixed_text += m_pending_llm_commit;
    } else {
      actions.insert("commit");
      std::string commit_text = escape_string<char>(wtou8(m_pending_llm_commit));
      messages.push_back(std::string("commit=") + commit_text + '\n');
    }

    // 清空待提交的LLM候选词
    m_pending_llm_commit.clear();
  }

  // 中英混打结束：整段送出
  if (!session_status.mixed_commit.empty()) {
    actions.insert("commit");
    messages.push_back(std::string("commit=") +
                       escape_string<char>(wtou8(session_status.mixed_commit)) + '\n');
    if (m_context_history)
      m_context_history->AddText(session_status.mixed_commit, m_dev_console);
    if (m_personal)
      m_personal->Record(m_context_history ? m_context_history->GetActiveKey() : L"",
                         session_status.mixed_commit);
    session_status.mixed_commit.clear();
  }

  RIME_STRUCT(RimeCommit, commit);
  if (session_status.mixed_active() && rime_api->get_commit(session_id, &commit)) {
    // 混打中 Rime 送出的字（选字、标点）收进混打内容
    if (commit.text)
      session_status.mixed_text += u8tow(commit.text);
    rime_api->free_commit(&commit);
  } else if (rime_api->get_commit(session_id, &commit)) {
    actions.insert("commit");

    std::string commit_text = escape_string<char>(commit.text);
    messages.push_back(std::string("commit=") + commit_text + '\n');
    
    // 记录用户提交的文本到上下文历史
    // 使用原始的commit.text而不是转义后的文本，确保正确记录
    if (commit.text && strlen(commit.text) > 0) {
      std::wstring commit_text_w = u8tow(commit.text);
      if (!commit_text_w.empty()) {
        if (m_context_history) {
          m_context_history->AddText(commit_text_w, m_dev_console);
        }
        if (m_personal)
          m_personal->Record(m_context_history ? m_context_history->GetActiveKey() : L"",
                             commit_text_w);

        LOG(INFO) << "[LLM] User committed text: " << commit.text;
        
        // 仅当 commit 包含有意义内容（非纯标点/符号）时才进入 LLM 预测模式，避免退出后输入标点又误入
        if (m_llm_after_commit && _PredictionAvailable() &&
            !m_llm_prediction_mode && CommitHasMeaningfulContent(commit_text_w)) {
          if (m_dev_console && m_dev_console->IsEnabled()) {
            m_dev_console->WriteLine(L"[LLM] Detected user commit, entering LLM prediction mode");
          }
          LOG(INFO) << "[LLM] Entering LLM prediction mode";
          m_llm_prediction_mode = true;
          // 调用LLM预测（上下文从 m_context_history 获取）
          _TriggerLLMPrediction(ipc_id);
          // 确保UI更新以显示LLM候选词
          // 注意：这里不能直接调用_UpdateUI，因为_Respond还在执行中
          // 需要在_Respond结束后调用_UpdateUI
        } else {
          if (!m_llm_provider) {
            LOG(WARNING) << "[LLM] LLM provider is not available";
          } else if (!m_llm_provider->IsAvailable()) {
            LOG(WARNING) << "[LLM] LLM provider is not enabled";
          } else if (m_llm_prediction_mode) {
            LOG(INFO) << "[LLM] Already in LLM prediction mode";
          }
        }
      }
    }
    
    rime_api->free_commit(&commit);
  }

  bool is_composing = false;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    is_composing = !!status.is_composing;
    actions.insert("status");
    messages.push_back(std::string("status.ascii_mode=") +
                       std::to_string(status.is_ascii_mode) + '\n');
    messages.push_back(std::string("status.composing=") +
                       std::to_string(status.is_composing || session_status.mixed_active()) +
                       '\n');
    messages.push_back(std::string("status.disabled=") +
                       std::to_string(status.is_disabled) + '\n');
    messages.push_back(std::string("status.full_shape=") +
                       std::to_string(status.is_full_shape) + '\n');
    messages.push_back(std::string("status.schema_id=") +
                       std::string(status.schema_id) + '\n');
    if (m_global_ascii_mode &&
        (session_status.status.is_ascii_mode != status.is_ascii_mode)) {
      for (auto& pair : m_session_status_map) {
        if (pair.first != ipc_id)
          rime_api->set_option(to_session_id(pair.first), "ascii_mode",
                               !!status.is_ascii_mode);
      }
    }
    session_status.status = status;
    rime_api->free_status(&status);
  }

  RIME_STRUCT(RimeContext, ctx);
  if (!is_composing) {
    session_status.preview_input.clear();
    session_status.preview_units.clear();
    session_status.preview_lens.clear();
    session_status.focus = -1;
  }
  if (rime_api->get_context(session_id, &ctx)) {
    if (is_composing) {
      actions.insert("ctx");
      switch (session_status.style.preedit_type) {
        case UIStyle::PREVIEW:
          // 西文模式（Shift 的 inline_ascii）下組字是英文按鍵，不能當注音轉換
          if (ctx.commit_text_preview != NULL && !session_status.status.is_ascii_mode) {
            char schema_id[256] = {0};
            rime_api->get_current_schema(session_id, schema_id,
                                         sizeof(schema_id));
            const char* input = rime_api->get_input(session_id);
            const int hl = ctx.menu.highlighted_candidate_index;
            const char* cand = hl >= 0 && hl < ctx.menu.num_candidates
                                   ? ctx.menu.candidates[hl].text
                                   : nullptr;
            // 逐字選字：選好了（候選沒了、游標跑回句尾）或輸入改變，就取消框選
            const size_t caret_pos = rime_api->get_caret_pos(session_id);
            if (session_status.focus >= 0 &&
                (ctx.menu.num_candidates == 0 || session_status.focus_input != (input ? input : "") ||
                 session_status.focus_caret != caret_pos))
              session_status.focus = -1;
            ZhuyinPreview pv = BuildZhuyinPreview(
                ctx.commit_text_preview,
                ctx.composition.preedit ? ctx.composition.preedit : "",
                input ? input : "", caret_pos,
                cand ? cand : "", hl, _LoadZhuyinSpeller(rime_api, schema_id),
                session_status.preview_input, session_status.preview_units,
                session_status.preview_lens,
                session_status.focus >= 0);
            // without a chosen word the whole preedit is the selection
            if (pv.sel_start == pv.sel_end) {
              pv.sel_start = 0;
              pv.sel_end = (int)pv.text.size();
            }
            messages.push_back(std::string("ctx.preedit=") +
                               escape_string<char>(wtou8(pv.text)) + '\n');
            messages.push_back(std::string("ctx.preedit.cursor=") +
                               std::to_string(pv.sel_start) + ',' +
                               std::to_string(pv.sel_end) + ',' +
                               std::to_string(pv.cursor) + '\n');
            break;
          }
          // no preview, fall back to composition
        case UIStyle::COMPOSITION:
          messages.push_back(std::string("ctx.preedit=") +
                             escape_string<char>(ctx.composition.preedit) +
                             '\n');
          if (ctx.composition.sel_start <= ctx.composition.sel_end) {
            messages.push_back(
                std::string("ctx.preedit.cursor=") +
                std::to_string(utf8towcslen(ctx.composition.preedit,
                                            ctx.composition.sel_start)) +
                ',' +
                std::to_string(utf8towcslen(ctx.composition.preedit,
                                            ctx.composition.sel_end)) +
                ',' +
                std::to_string(utf8towcslen(ctx.composition.preedit,
                                            ctx.composition.cursor_pos)) +
                '\n');
          }
          break;
        case UIStyle::PREVIEW_ALL:
          CandidateInfo cinfo;
          _GetCandidateInfo(cinfo, ctx);
          std::string topush = std::string("ctx.preedit=") +
                               escape_string<char>(ctx.composition.preedit) +
                               "  [";
          for (auto i = 0; i < ctx.menu.num_candidates; i++) {
            std::string label =
                session_status.style.label_font_point > 0
                    ? _GetLabelText(
                          cinfo.labels, i,
                          session_status.style.label_text_format.c_str())
                    : "";
            std::string comment = session_status.style.comment_font_point > 0
                                      ? wtou8(cinfo.comments.at(i).str)
                                      : "";
            std::string mark_text = session_status.style.mark_text.empty()
                                        ? "*"
                                        : wtou8(session_status.style.mark_text);
            std::string prefix =
                (i != ctx.menu.highlighted_candidate_index) ? "" : mark_text;
            topush += " " + prefix + escape_string(label) +
                      escape_string<char>(ctx.menu.candidates[i].text) + " " +
                      escape_string(comment);
          }
          messages.push_back(topush + " ]\n");
          if (ctx.composition.sel_start <= ctx.composition.sel_end) {
            messages.push_back(
                std::string("ctx.preedit.cursor=") +
                std::to_string(utf8towcslen(ctx.composition.preedit,
                                            ctx.composition.sel_start)) +
                ',' +
                std::to_string(utf8towcslen(ctx.composition.preedit,
                                            ctx.composition.sel_end)) +
                ',' +
                std::to_string(utf8towcslen(ctx.composition.preedit,
                                            ctx.composition.cursor_pos)) +
                '\n');
          }
          break;
      }
    }
    // 如果有Rime候选词，或者处于LLM预测模式且有LLM候选词，序列化候选词信息
    if (ctx.menu.num_candidates || 
        (m_llm_prediction_mode && !m_current_llm_candidates.empty())) {
      CandidateInfo cinfo;
      std::wstringstream ss;
      boost::archive::text_woarchive oa(ss);
      _GetCandidateInfo(cinfo, ctx);

      oa << cinfo;

      messages.push_back(std::string("ctx.cand=") + wtou8(ss.str()) + '\n');
    }
    rime_api->free_context(&ctx);
  } else if (m_llm_prediction_mode && !m_current_llm_candidates.empty()) {
    // 如果没有Rime上下文但处于LLM预测模式，也需要序列化LLM候选词
    CandidateInfo cinfo;
    std::wstringstream ss;
    boost::archive::text_woarchive oa(ss);
    RimeContext empty_ctx = {0};
    _GetCandidateInfo(cinfo, empty_ctx);
    
    oa << cinfo;
    messages.push_back(std::string("ctx.cand=") + wtou8(ss.str()) + '\n');
  }

  // 中英混打：混打内容显示在组字区最前面，Rime 的组字（含选取范围与光标）接在后面
  if (session_status.mixed_active()) {
    std::string rime_preedit;
    int sel_start = 0, sel_end = 0, cursor = -1;
    for (auto it = messages.begin(); it != messages.end();) {
      if (it->rfind("ctx.preedit.cursor=", 0) == 0) {
        sscanf_s(it->c_str() + 19, "%d,%d,%d", &sel_start, &sel_end, &cursor);
        it = messages.erase(it);
      } else if (it->rfind("ctx.preedit=", 0) == 0) {
        rime_preedit = unescape_string(it->substr(12, it->size() - 13));
        it = messages.erase(it);
      } else {
        ++it;
      }
    }
    const std::wstring rime_w = u8tow(rime_preedit);
    const std::wstring shown = session_status.mixed_text + rime_w;
    const int offset = (int)session_status.mixed_text.size();
    const int total = (int)shown.size();
    if (cursor < 0 || rime_w.empty()) {
      sel_start = 0;
      sel_end = cursor = total;
    } else if (sel_start == 0 && sel_end == (int)rime_w.size()) {
      sel_end = total;  // 选取整段组字时，混打内容也一起算，不另外标示
      cursor += offset;
    } else {
      sel_start += offset;
      sel_end += offset;
      cursor += offset;
    }
    actions.insert("ctx");
    messages.push_back(std::string("ctx.preedit=") + escape_string<char>(wtou8(shown)) + '\n');
    messages.push_back(std::string("ctx.preedit.cursor=") + std::to_string(sel_start) + ',' +
                       std::to_string(sel_end) + ',' + std::to_string(cursor) + '\n');
  }

  // configuration information
  actions.insert("config");
  messages.push_back(std::string("config.inline_preedit=") +
                     std::to_string((int)session_status.style.inline_preedit) +
                     '\n');

  // style
  if (!session_status.__synced) {
    messages.push_back(std::string("config.hide_ime_mode_icon=") +
                       std::to_string((int)hide_ime_mode_icon) + "\n");
    std::wstringstream ss;
    boost::archive::text_woarchive oa(ss);
    oa << session_status.style;

    actions.insert("style");
    messages.push_back(std::string("style=") + wtou8(ss.str().c_str()) + '\n');
    session_status.__synced = true;
  }

  // summarize

  if (actions.empty()) {
    messages.insert(messages.begin(), std::string("action=noop\n"));
  } else {
    std::string actionList(join(actions, ","));
    messages.insert(messages.begin(),
                    std::string("action=") + actionList + '\n');
  }

  messages.push_back(std::string(".\n"));

  return std::all_of(messages.begin(), messages.end(),
                     [&eat](std::string& msg) {
                       auto wmsg = u8tow(msg);
                       return eat(wmsg);
                     });
}

static inline COLORREF blend_colors(COLORREF fcolor, COLORREF bcolor) {
  // 提取各通道的值
  BYTE fA = (fcolor >> 24) & 0xFF;  // 获取前景的 alpha 通道
  BYTE fB = (fcolor >> 16) & 0xFF;  // 获取前景的 blue 通道
  BYTE fG = (fcolor >> 8) & 0xFF;   // 获取前景的 green 通道
  BYTE fR = fcolor & 0xFF;          // 获取前景的 red 通道
  BYTE bA = (bcolor >> 24) & 0xFF;  // 获取背景的 alpha 通道
  BYTE bB = (bcolor >> 16) & 0xFF;  // 获取背景的 blue 通道
  BYTE bG = (bcolor >> 8) & 0xFF;   // 获取背景的 green 通道
  BYTE bR = bcolor & 0xFF;          // 获取背景的 red 通道
  // 将 alpha 通道转换为 [0, 1] 的浮动值
  float fAlpha = fA / 255.0f;
  float bAlpha = bA / 255.0f;
  // 计算每个通道的加权平均值
  float retAlpha = fAlpha + (1 - fAlpha) * bAlpha;
  // 混合红、绿、蓝通道
  BYTE retR = (BYTE)((fR * fAlpha + bR * bAlpha * (1 - fAlpha)) / retAlpha);
  BYTE retG = (BYTE)((fG * fAlpha + bG * bAlpha * (1 - fAlpha)) / retAlpha);
  BYTE retB = (BYTE)((fB * fAlpha + bB * bAlpha * (1 - fAlpha)) / retAlpha);
  // 返回合成后的颜色
  return (BYTE)(retAlpha * 255) << 24 | retB << 16 | retG << 8 | retR;
}
// parse color value, with fallback value
static Bool _RimeGetColor(RimeConfig* config,
                          const std::string key,
                          int& value,
                          const ColorFormat& fmt,
                          const unsigned int& fallback) {
  RimeApi* rime_api = rime_get_api();
  char color[256] = {0};
  if (!rime_api->config_get_string(config, key.c_str(), color, 256)) {
    value = fallback;
    return False;
  }
  const auto color_str = std::string(color);
  const auto make_opaque = [&](int& value) {
    value = (fmt != COLOR_RGBA) ? (value | 0xff000000)
                                : ((value << 8) | 0x000000ff);
  };
  const auto ConvertColorToAbgr = [](int color, ColorFormat fmt = COLOR_ABGR) {
    if (fmt == COLOR_ABGR)
      return color & 0xffffffff;
    else if (fmt == COLOR_ARGB)
      return ARGB2ABGR(color) & 0xffffffff;
    else
      return RGBA2ABGR(color) & 0xffffffff;
  };
  if (std::regex_match(color_str, HEX_REGEX)) {
    auto tmp = std::regex_replace(color_str, TRIMHEAD_REGEX, "").substr(0, 8);
    switch (tmp.length()) {
      case 6:  // color code without alpha, xxyyzz add alpha ff
        value = std::stoul(tmp, 0, 16);
        make_opaque(value);
        break;
      case 3:  // color hex code xyz => xxyyzz and alpha ff
        tmp = std::string(2, tmp[0]) + std::string(2, tmp[1]) +
              std::string(2, tmp[2]);
        value = std::stoul(tmp, 0, 16);
        make_opaque(value);
        break;
      case 4:  // color hex code vxyz => vvxxyyzz
        tmp = std::string(2, tmp[0]) + std::string(2, tmp[1]) +
              std::string(2, tmp[2]) + std::string(2, tmp[3]);
        value = std::stoul(tmp, 0, 16);
        break;
      case 7:
      case 8:  // color code with alpha
        value = std::stoul(tmp, 0, 16);
        break;
      default:  // invalid length
        value = fallback;
        return False;
    }
  } else {
    int tmp = 0;
    if (!rime_api->config_get_int(config, key.c_str(), &tmp)) {
      value = fallback;
      return False;
    } else
      value = tmp;
    make_opaque(value);
  }
  value = ConvertColorToAbgr(value, fmt);
  return True;
}
// parset bool type configuration to T type value trueValue / falseValue
template <typename T>
void _RimeGetBool(RimeConfig* config,
                  const char* key,
                  bool cond,
                  T& value,
                  const T& trueValue = true,
                  const T& falseValue = false) {
  RimeApi* rime_api = rime_get_api();
  Bool tempb = False;
  if (rime_api->config_get_bool(config, key, &tempb) || cond)
    value = (!!tempb) ? trueValue : falseValue;
}
//	parse string option to T type value, with fallback
template <typename T>
void _RimeParseStringOptWithFallback(RimeConfig* config,
                                     const std::string& key,
                                     T& value,
                                     const std::map<std::string, T>& amap,
                                     const T& fallback) {
  RimeApi* rime_api = rime_get_api();
  char str_buff[256] = {0};
  if (rime_api->config_get_string(config, key.c_str(), str_buff, 255)) {
    auto it = amap.find(std::string(str_buff));
    value = (it != amap.end()) ? it->second : fallback;
  } else
    value = fallback;
}

template <typename T>
void _RimeGetIntStr(RimeConfig* config,
                    const char* key,
                    T& value,
                    const char* fb_key = nullptr,
                    const void* fb_value = nullptr,
                    const std::function<void(T&)>& func = nullptr) {
  RimeApi* rime_api = rime_get_api();
  if constexpr (std::is_same<T, int>::value) {
    if (!rime_api->config_get_int(config, key, &value) && fb_key != 0)
      rime_api->config_get_int(config, fb_key, &value);
  } else if constexpr (std::is_same<T, std::wstring>::value) {
    const int BUF_SIZE = 2047;
    char buffer[BUF_SIZE + 1] = {0};
    if (rime_api->config_get_string(config, key, buffer, BUF_SIZE) ||
        rime_api->config_get_string(config, fb_key, buffer, BUF_SIZE)) {
      value = u8tow(buffer);
    } else if (fb_value) {
      value = *(T*)fb_value;
    }
  }
  if (func)
    func(value);
}

void RimeWithWeaselHandler::_UpdateShowNotifications(RimeConfig* config,
                                                     bool initialize) {
  Bool show_notifications = true;
  RimeConfigIterator iter;
  if (initialize)
    m_show_notifications_base.clear();
  m_show_notifications.clear();

  if (rime_api->config_get_bool(config, "show_notifications",
                                &show_notifications)) {
    // config read as bool, for gloal all on or off
    if (show_notifications)
      m_show_notifications["always"] = true;
    if (initialize)
      m_show_notifications_base = m_show_notifications;
  } else if (rime_api->config_begin_list(&iter, config, "show_notifications")) {
    // config read as list, list item should be option name in schema
    // or key word 'schema' for schema switching tip
    while (rime_api->config_next(&iter)) {
      char buffer[256] = {0};
      if (rime_api->config_get_string(config, iter.path, buffer, 256))
        m_show_notifications[std::string(buffer)] = true;
    }
    if (initialize)
      m_show_notifications_base = m_show_notifications;
    rime_api->config_end(&iter);
  } else {
    // not configured, or incorrect type
    if (initialize)
      m_show_notifications_base["always"] = true;
    m_show_notifications = m_show_notifications_base;
  }
}

// update ui's style parameters, ui has been check before referenced
static void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize) {
  UIStyle& style(ui->style());
  const std::function<void(std::wstring&)> rmspace = [](std::wstring& str) {
    str = std::regex_replace(str, std::wregex(L"\\s*(,|:|^|$)\\s*"), L"$1");
  };
  const std::function<void(int&)> _abs = [](int& value) { value = abs(value); };
  // get font faces
  _RimeGetIntStr(config, "style/font_face", style.font_face, 0, 0, rmspace);
  std::wstring* const pFallbackFontFace = initialize ? &style.font_face : NULL;
  _RimeGetIntStr(config, "style/label_font_face", style.label_font_face, 0,
                 pFallbackFontFace, rmspace);
  _RimeGetIntStr(config, "style/comment_font_face", style.comment_font_face, 0,
                 pFallbackFontFace, rmspace);
  // able to set label font/comment font empty, force fallback to font face.
  if (style.label_font_face.empty())
    style.label_font_face = style.font_face;
  if (style.comment_font_face.empty())
    style.comment_font_face = style.font_face;
  // get font points
  _RimeGetIntStr(config, "style/font_point", style.font_point);
  if (style.font_point <= 0)
    style.font_point = 12;
  _RimeGetBool(config, "hide_ime_mode_icon", initialize, hide_ime_mode_icon);
  _RimeGetIntStr(config, "style/label_font_point", style.label_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/comment_font_point", style.comment_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/candidate_abbreviate_length",
                 style.candidate_abbreviate_length, 0, 0, _abs);
  _RimeGetBool(config, "style/inline_preedit", initialize,
               style.inline_preedit);
  _RimeGetBool(config, "style/vertical_auto_reverse", initialize,
               style.vertical_auto_reverse);
  const std::map<std::string, UIStyle::PreeditType> _preeditMap = {
      {std::string("composition"), UIStyle::COMPOSITION},
      {std::string("preview"), UIStyle::PREVIEW},
      {std::string("preview_all"), UIStyle::PREVIEW_ALL}};
  _RimeParseStringOptWithFallback(config, "style/preedit_type",
                                  style.preedit_type, _preeditMap,
                                  style.preedit_type);
  const std::map<std::string, UIStyle::AntiAliasMode> _aliasModeMap = {
      {std::string("force_dword"), UIStyle::FORCE_DWORD},
      {std::string("cleartype"), UIStyle::CLEARTYPE},
      {std::string("grayscale"), UIStyle::GRAYSCALE},
      {std::string("aliased"), UIStyle::ALIASED},
      {std::string("default"), UIStyle::DEFAULT}};
  _RimeParseStringOptWithFallback(config, "style/antialias_mode",
                                  style.antialias_mode, _aliasModeMap,
                                  style.antialias_mode);
  const std::map<std::string, UIStyle::HoverType> _hoverTypeMap = {
      {std::string("none"), UIStyle::HoverType::NONE},
      {std::string("semi_hilite"), UIStyle::HoverType::SEMI_HILITE},
      {std::string("hilite"), UIStyle::HoverType::HILITE}};
  _RimeParseStringOptWithFallback(config, "style/hover_type", style.hover_type,
                                  _hoverTypeMap, style.hover_type);
  const std::map<std::string, UIStyle::LayoutAlignType> _alignType = {
      {std::string("top"), UIStyle::ALIGN_TOP},
      {std::string("center"), UIStyle::ALIGN_CENTER},
      {std::string("bottom"), UIStyle::ALIGN_BOTTOM}};
  _RimeParseStringOptWithFallback(config, "style/layout/align_type",
                                  style.align_type, _alignType,
                                  style.align_type);
  _RimeGetBool(config, "style/display_tray_icon", initialize,
               style.display_tray_icon);
  _RimeGetBool(config, "style/ascii_tip_follow_cursor", initialize,
               style.ascii_tip_follow_cursor);
  _RimeGetBool(config, "style/horizontal", initialize, style.layout_type,
               UIStyle::LAYOUT_HORIZONTAL, UIStyle::LAYOUT_VERTICAL);
  _RimeGetBool(config, "style/paging_on_scroll", initialize,
               style.paging_on_scroll);
  _RimeGetBool(config, "style/click_to_capture", initialize,
               style.click_to_capture, true, false);
  _RimeGetBool(config, "style/fullscreen", false, style.layout_type,
               ((style.layout_type == UIStyle::LAYOUT_HORIZONTAL)
                    ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                    : UIStyle::LAYOUT_VERTICAL_FULLSCREEN),
               style.layout_type);
  _RimeGetBool(config, "style/vertical_text", false, style.layout_type,
               UIStyle::LAYOUT_VERTICAL_TEXT, style.layout_type);
  _RimeGetBool(config, "style/vertical_text_left_to_right", false,
               style.vertical_text_left_to_right);
  _RimeGetBool(config, "style/vertical_text_with_wrap", false,
               style.vertical_text_with_wrap);
  const std::map<std::string, bool> _text_orientation = {
      {std::string("horizontal"), false}, {std::string("vertical"), true}};
  bool _text_orientation_bool = false;
  _RimeParseStringOptWithFallback(config, "style/text_orientation",
                                  _text_orientation_bool, _text_orientation,
                                  _text_orientation_bool);
  if (_text_orientation_bool)
    style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT;
  _RimeGetIntStr(config, "style/label_format", style.label_text_format);
  _RimeGetIntStr(config, "style/mark_text", style.mark_text);
  _RimeGetIntStr(config, "style/layout/baseline", style.baseline, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/linespacing", style.linespacing, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/min_width", style.min_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/max_width", style.max_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/min_height", style.min_height, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/max_height", style.max_height, 0, 0,
                 _abs);
  // layout (alternative to style/horizontal)
  const std::map<std::string, UIStyle::LayoutType> _layoutMap = {
      {std::string("vertical"), UIStyle::LAYOUT_VERTICAL},
      {std::string("horizontal"), UIStyle::LAYOUT_HORIZONTAL},
      {std::string("vertical_text"), UIStyle::LAYOUT_VERTICAL_TEXT},
      {std::string("vertical+fullscreen"), UIStyle::LAYOUT_VERTICAL_FULLSCREEN},
      {std::string("horizontal+fullscreen"),
       UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN}};
  _RimeParseStringOptWithFallback(config, "style/layout/type",
                                  style.layout_type, _layoutMap,
                                  style.layout_type);
  // disable max_width when full screen
  if (style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
      style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) {
    style.max_width = 0;
    style.inline_preedit = false;
  }
  _RimeGetIntStr(config, "style/layout/border", style.border,
                 "style/layout/border_width", 0, _abs);
  _RimeGetIntStr(config, "style/layout/margin_x", style.margin_x);
  _RimeGetIntStr(config, "style/layout/margin_y", style.margin_y);
  _RimeGetIntStr(config, "style/layout/spacing", style.spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/candidate_spacing",
                 style.candidate_spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_spacing", style.hilite_spacing, 0,
                 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_x",
                 style.hilite_padding_x, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_y",
                 style.hilite_padding_y, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/shadow_radius", style.shadow_radius, 0,
                 0, _abs);
  // disable shadow for fullscreen layout
  style.shadow_radius *=
      (!(style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
         style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN));
  _RimeGetIntStr(config, "style/layout/shadow_offset_x", style.shadow_offset_x);
  _RimeGetIntStr(config, "style/layout/shadow_offset_y", style.shadow_offset_y);
  // round_corner as alias of hilited_corner_radius
  _RimeGetIntStr(config, "style/layout/hilited_corner_radius",
                 style.round_corner, "style/layout/round_corner", 0, _abs);
  // corner_radius not set, fallback to round_corner
  _RimeGetIntStr(config, "style/layout/corner_radius", style.round_corner_ex,
                 "style/layout/round_corner", 0, _abs);
  // fix padding and spacing settings
  if (style.layout_type != UIStyle::LAYOUT_VERTICAL_TEXT) {
    // hilite_padding vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = max(style.spacing, style.hilite_padding_y * 2);
    // hilite_padding vs candidate_spacing
    if (style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN ||
        style.layout_type == UIStyle::LAYOUT_VERTICAL) {
      // vertical, if hilite_padding_y over candidate spacing,
      // increase candidate spacing
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_y * 2);
    } else {
      // horizontal, if hilite_padding_x over candidate
      // spacing, increase candidate spacing
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_x * 2);
    }
    // hilite_padding_x vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = max(style.hilite_spacing, style.hilite_padding_x);
  } else  // LAYOUT_VERTICAL_TEXT
  {
    // hilite_padding_x vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = max(style.spacing, style.hilite_padding_x * 2);
    // hilite_padding vs candidate_spacing
    // if hilite_padding_x over candidate
    // spacing, increase candidate spacing
    style.candidate_spacing =
        max(style.candidate_spacing, style.hilite_padding_x * 2);
    // vertical_text_with_wrap and hilite_padding_y over candidate_spacing
    if (style.vertical_text_with_wrap)
      style.candidate_spacing =
          max(style.candidate_spacing, style.hilite_padding_y * 2);
    // hilite_padding_y vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = max(style.hilite_spacing, style.hilite_padding_y);
  }
  // fix padding and margin settings
  int scale = style.margin_x < 0 ? -1 : 1;
  style.margin_x = scale * max(style.hilite_padding_x, abs(style.margin_x));
  scale = style.margin_y < 0 ? -1 : 1;
  style.margin_y = scale * max(style.hilite_padding_y, abs(style.margin_y));
  // get enhanced_position
  _RimeGetBool(config, "style/enhanced_position", initialize,
               style.enhanced_position, true, false);
  // get color scheme
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  if (initialize && rime_api->config_get_string(config, "style/color_scheme",
                                                buffer, BUF_SIZE))
    _UpdateUIStyleColor(config, style);
}
// load color configs to style, by "style/color_scheme" or specific scheme name
// "color" which is default empty
static bool _UpdateUIStyleColor(RimeConfig* config,
                                UIStyle& style,
                                std::string color) {
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  std::string color_mark = "style/color_scheme";
  // color scheme
  if (rime_api->config_get_string(config, color_mark.c_str(), buffer,
                                  BUF_SIZE) ||
      !color.empty()) {
    std::string prefix("preset_color_schemes/");
    prefix += (color.empty()) ? buffer : color;
    // define color format, default abgr if not set
    ColorFormat fmt = COLOR_ABGR;
    const std::map<std::string, ColorFormat> _colorFmt = {
        {std::string("argb"), COLOR_ARGB},
        {std::string("rgba"), COLOR_RGBA},
        {std::string("abgr"), COLOR_ABGR}};
    _RimeParseStringOptWithFallback(config, (prefix + "/color_format"), fmt,
                                    _colorFmt, COLOR_ABGR);
#define COLOR(key, value, fallback) \
  _RimeGetColor(config, (prefix + "/" + key), value, fmt, fallback)
    COLOR("back_color", style.back_color, 0xffffffff);
    COLOR("shadow_color", style.shadow_color, 0);
    COLOR("prevpage_color", style.prevpage_color, 0);
    COLOR("nextpage_color", style.nextpage_color, 0);
    COLOR("text_color", style.text_color, 0xff000000);
    COLOR("candidate_text_color", style.candidate_text_color, style.text_color);
    COLOR("candidate_back_color", style.candidate_back_color, 0);
    COLOR("border_color", style.border_color, style.text_color);
    COLOR("hilited_text_color", style.hilited_text_color, style.text_color);
    COLOR("hilited_back_color", style.hilited_back_color, style.back_color);
    COLOR("hilited_candidate_text_color", style.hilited_candidate_text_color,
          style.hilited_text_color);
    COLOR("hilited_candidate_back_color", style.hilited_candidate_back_color,
          style.hilited_back_color);
    COLOR("hilited_candidate_shadow_color",
          style.hilited_candidate_shadow_color, 0);
    COLOR("hilited_shadow_color", style.hilited_shadow_color, 0);
    COLOR("candidate_shadow_color", style.candidate_shadow_color, 0);
    COLOR("candidate_border_color", style.candidate_border_color, 0);
    COLOR("hilited_candidate_border_color",
          style.hilited_candidate_border_color, 0);
    COLOR("label_color", style.label_text_color,
          blend_colors(style.candidate_text_color, style.candidate_back_color));
    COLOR("hilited_label_color", style.hilited_label_text_color,
          blend_colors(style.hilited_candidate_text_color,
                       style.hilited_candidate_back_color));
    COLOR("comment_text_color", style.comment_text_color,
          style.label_text_color);
    COLOR("hilited_comment_text_color", style.hilited_comment_text_color,
          style.hilited_label_text_color);
    COLOR("hilited_mark_color", style.hilited_mark_color, 0);
#undef COLOR
    return true;
  }
  return false;
}

static void _LoadAppOptions(RimeConfig* config,
                            AppOptionsByAppName& app_options) {
  app_options.clear();
  RimeConfigIterator app_iter;
  RimeConfigIterator option_iter;
  rime_api->config_begin_map(&app_iter, config, "app_options");
  while (rime_api->config_next(&app_iter)) {
    AppOptions& options(app_options[app_iter.key]);
    rime_api->config_begin_map(&option_iter, config, app_iter.path);
    while (rime_api->config_next(&option_iter)) {
      Bool value = False;
      if (rime_api->config_get_bool(config, option_iter.path, &value)) {
        options[option_iter.key] = !!value;
      }
    }
    rime_api->config_end(&option_iter);
  }
  rime_api->config_end(&app_iter);
}

void RimeWithWeaselHandler::_GetStatus(Status& stat,
                                       WeaselSessionId ipc_id,
                                       Context& ctx) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = "";
    if (status.schema_id)
      schema_id = status.schema_id;
    stat.schema_name = u8tow(status.schema_name);
    stat.schema_id = u8tow(status.schema_id);
    stat.ascii_mode = !!status.is_ascii_mode;
    stat.composing = !!status.is_composing || session_status.mixed_active();
    
    // 如果处于LLM预测模式，强制设置composing为true以显示候选栏
    if (m_llm_prediction_mode && !m_current_llm_candidates.empty()) {
      stat.composing = true;
      if (m_dev_console && m_dev_console->IsEnabled()) {
        m_dev_console->WriteLine(L"[_GetStatus] LLM预测模式激活，强制设置 composing=true");
      }
    }
    
    stat.disabled = !!status.is_disabled;
    stat.full_shape = !!status.is_full_shape;
    if (schema_id != m_last_schema_id) {
      session_status.__synced = false;
      m_last_schema_id = schema_id;
      if (schema_id != ".default") {  // don't load for schema select menu
        bool inline_preedit = session_status.style.inline_preedit;
        _LoadSchemaSpecificSettings(ipc_id, schema_id);
        _LoadAppInlinePreeditSet(ipc_id, true);
        if (session_status.style.inline_preedit != inline_preedit)
          // in case of inline_preedit set in schema
          _UpdateInlinePreeditStatus(ipc_id);
        // refresh icon after schema changed
        _RefreshTrayIcon(session_id, _UpdateUICallback);
        m_ui->style() = session_status.style;
        if (m_show_notifications.find("schema") != m_show_notifications.end() &&
            m_show_notifications_time > 0) {
          ctx.aux.str = stat.schema_name;
          m_ui->Update(ctx, stat);
          m_ui->ShowWithTimeout(m_show_notifications_time);
        }
      }
    }
    rime_api->free_status(&status);
  }
}

void RimeWithWeaselHandler::_GetContext(Context& weasel_context,
                                        RimeSessionId session_id) {
  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    if (ctx.composition.length > 0) {
      weasel_context.preedit.str = u8tow(ctx.composition.preedit);
      if (ctx.composition.sel_start < ctx.composition.sel_end) {
        TextAttribute attr;
        attr.type = HIGHLIGHTED;
        attr.range.start =
            utf8towcslen(ctx.composition.preedit, ctx.composition.sel_start);
        attr.range.end =
            utf8towcslen(ctx.composition.preedit, ctx.composition.sel_end);

        weasel_context.preedit.attributes.push_back(attr);
      }
    }
    
    // 获取候选词信息（包括Rime和LLM候选词）
      CandidateInfo& cinfo(weasel_context.cinfo);
    if (ctx.menu.num_candidates > 0) {
      // 有Rime候选词，调用_GetCandidateInfo会同时添加Rime和LLM候选词
      _GetCandidateInfo(cinfo, ctx);
      if (m_dev_console && m_dev_console->IsEnabled()) {
        std::wstringstream ss;
        ss << L"[DEBUG] _GetContext: 有Rime候选词(" << ctx.menu.num_candidates 
           << L"个)，添加后总候选词数=" << cinfo.candies.size();
        m_dev_console->WriteLine(ss.str());
      }
    } else if (m_llm_prediction_mode && !m_current_llm_candidates.empty()) {
      // 如果处于LLM预测模式但没有Rime候选词，只显示LLM候选词
      cinfo.clear();
      _GetCandidateInfo(cinfo, ctx);  // 这会添加LLM候选词
      if (m_dev_console && m_dev_console->IsEnabled()) {
        std::wstringstream ss;
        ss << L"[DEBUG] _GetContext: 无Rime候选词，添加LLM候选词后 cinfo.candies.size()=" 
           << cinfo.candies.size();
        m_dev_console->WriteLine(ss.str());
      }
    } else {
      // 既没有Rime候选词，也没有LLM候选词，清空候选词信息
      cinfo.clear();
    }
    
    rime_api->free_context(&ctx);
  } else if (m_llm_prediction_mode && !m_current_llm_candidates.empty()) {
    // 如果没有Rime上下文但处于LLM预测模式，创建空的候选词信息并添加LLM候选词
    CandidateInfo& cinfo(weasel_context.cinfo);
    cinfo.clear();
    RimeContext empty_ctx = {0};
    _GetCandidateInfo(cinfo, empty_ctx);  // 这会添加LLM候选词
    if (m_dev_console && m_dev_console->IsEnabled()) {
      std::wstringstream ss;
      ss << L"[DEBUG] _GetContext: 无Rime上下文，添加LLM候选词后 cinfo.candies.size()=" 
         << cinfo.candies.size();
      m_dev_console->WriteLine(ss.str());
    }
  }
}

bool RimeWithWeaselHandler::_IsSessionTSF(RimeSessionId session_id) {
  static char client_type[20] = {0};
  rime_api->get_property(session_id, "client_type", client_type,
                         sizeof(client_type) - 1);
  return std::string(client_type) == "tsf";
}

void RimeWithWeaselHandler::_UpdateInlinePreeditStatus(WeaselSessionId ipc_id) {
  if (!m_ui)
    return;
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set inline_preedit option
  bool inline_preedit =
      session_status.style.inline_preedit && _IsSessionTSF(session_id);
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}

void RimeWithWeaselHandler::SetContextHistory(ContextHistory* context_history) {
  m_context_history = context_history;
}

void RimeWithWeaselHandler::SetDevConsole(DevConsole* dev_console) {
  m_dev_console = dev_console;
  // 设置全局开发终端实例供LLMProvider使用
  extern DevConsole* g_dev_console;
  g_dev_console = dev_console;
  
  // 输出LLM提供者状态
  if (m_dev_console && m_dev_console->IsEnabled()) {
    if (!m_llm_provider) {
      m_dev_console->WriteLine(L"[LLM] LLM提供者未初始化");
      m_dev_console->WriteLine(L"[LLM] 请在weasel.yaml中配置：");
      m_dev_console->WriteLine(L"[LLM]   llm:");
      m_dev_console->WriteLine(L"[LLM]     enabled: true");
      m_dev_console->WriteLine(L"[LLM]     openai:");
      m_dev_console->WriteLine(L"[LLM]       api_key: \"your-api-key\"");
    } else if (!m_llm_provider->IsAvailable()) {
      m_dev_console->WriteLine(L"[LLM] LLM提供者已初始化，但不可用");
      m_dev_console->WriteLine(L"[LLM] 请检查配置：llm/enabled 和 llm/openai/api_key");
    } else {
      std::wstring provider_name = u8tow(m_llm_provider->GetProviderName());
      m_dev_console->WriteLine(L"[LLM] LLM提供者已就绪: " + provider_name);
    }
  }
}

// 清洗模型输出：只保留第一行，去除控制字符与首尾引号/括号/标点，去重，丢弃空候选
// （小模型常模仿 prompt 中的 "…" 格式，并在多路采样时给出相同结果）。
// prefix 非空时为输入中补全：候选 = Rime 当前转换结果 + 续写，选中即整段提交。
static std::vector<std::wstring> CleanLLMCandidates(const std::vector<std::wstring>& raw_list,
                                                    const std::wstring& prefix) {
  static const wchar_t kTrimChars[] =
      L" \t　\"'`“”‘’「」『』"
      L"()[]{}<>（）【】《》"
      L",.;:!?，。、；：！？…";
  std::vector<std::wstring> cleaned;
  for (const auto& raw : raw_list) {
    std::wstring s = raw.substr(0, raw.find_first_of(L"\r\n"));
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](wchar_t c) { return c < 0x20 || c == 0x7f; }),
            s.end());
    const size_t b = s.find_first_not_of(kTrimChars);
    s = (b == std::wstring::npos)
            ? std::wstring()
            : s.substr(b, s.find_last_not_of(kTrimChars) - b + 1);
    if (!s.empty())
      s = prefix + s;
    if (!s.empty() && std::find(cleaned.begin(), cleaned.end(), s) == cleaned.end())
      cleaned.push_back(std::move(s));
  }
  return cleaned;
}

void RimeWithWeaselHandler::PersonalCommand(DWORD command) {
  // 1 更新狀態 2 精煉 3 重新精煉全部 4 清除 5 匯出詞彙 6 套用修改並匯出 7 產生 Rime 詞典；
  // 狀態寫在 personal/status.txt
  if (!m_personal || !m_refiner) {
    const std::filesystem::path dir = WeaselUserDataPath() / L"personal";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(dir / L"status.txt", std::ios::binary | std::ios::trunc);
    f << "disabled=1\n";
    return;
  }
  switch (command) {
    case 2:
    case 3:
      if (!m_refiner->RunAsync(command == 3))
        m_refiner->WriteStatus();
      break;
    case 4:
      if (!m_refiner->Clear())
        m_refiner->WriteStatus();
      break;
    case 6: {
      // 設定程式的「詞庫管理」：套用 personal/edit.dat（加密）後重新匯出
      const std::filesystem::path edit = m_personal->Dir() / L"edit.dat";
      m_personal->ApplyEdits(edit);
      std::error_code ec;
      std::filesystem::remove(edit, ec);
    }
      [[fallthrough]];
    case 7:
      // 設定程式開啟「注音排序」時：先產生 Rime 詞典，設定程式接著重新部署
      m_refiner->ExportRimeDict();
      m_refiner->WriteStatus();
      break;
    case 5:
      // 匯出詞彙與精煉規則到 personal/export.dat（加密）
      m_personal->ExportTo(m_personal->Dir() / L"export.dat", 20000);
      m_refiner->WriteStatus();
      break;
    default:
      m_refiner->WriteStatus();
  }
}

void RimeWithWeaselHandler::LLMTestRequest() {
  // 在后台线程执行：推理可能要几百毫秒，不能占住 IPC 锁卡住所有应用的打字
  std::thread([this]() {
    const std::filesystem::path dir = WeaselUserDataPath();
    std::string request;
    {
      std::ifstream in(dir / L"llm_test_request.txt", std::ios::binary);
      request.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // 第一行是请求编号，其余是前文（空 = 只查询状态）
    const size_t nl = request.find('\n');
    std::string id = request.substr(0, nl);
    while (!id.empty() && (id.back() == '\r' || id.back() == ' '))
      id.pop_back();
    std::wstring context = nl == std::string::npos ? L"" : u8tow(request.substr(nl + 1));
    while (!context.empty() && (context.back() == L'\r' || context.back() == L'\n'))
      context.pop_back();

    std::ostringstream out;
    out << "id=" << id << "\n";
    {
      std::lock_guard<std::mutex> infer_lock(m_llm_infer_mutex);
      out << "model=" << wtou8(m_llm_loaded_model) << "\n";
      if (!m_llm_provider || !m_llm_provider->IsAvailable()) {
        out << "status=disabled\n";
      } else if (context.empty()) {
        out << "status=ok\n";
      } else {
        const ULONGLONG t0 = GetTickCount64();
        auto raw = m_llm_provider->PredictCandidates(context, L"", 5);
        const ULONGLONG ms = GetTickCount64() - t0;
        out << "status=ok\nms=" << ms << "\n";
        for (const auto& c : CleanLLMCandidates(raw, L""))
          out << "cand=" << wtou8(c) << "\n";
      }
    }
    // 先写临时文件再替换，避免设定画面读到写了一半的内容
    const std::filesystem::path tmp = dir / L"llm_test_response.tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f << out.str();
    }
    MoveFileExW(tmp.c_str(), (dir / L"llm_test_response.txt").c_str(),
                MOVEFILE_REPLACE_EXISTING);
  }).detach();
}

// 整句校正结果：去掉首尾空白、引号与句末标点；与初稿相同（不必校正）或字数差太多（模型没照做）时丢弃
static std::wstring CleanCorrection(const std::wstring& raw, const std::wstring& draft) {
  static const wchar_t kTrimChars[] = L" \t　\"'`“”‘’「」『』。.";
  std::wstring s = raw.substr(0, raw.find_first_of(L"\r\n"));
  s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c < 0x20 || c == 0x7f; }),
          s.end());
  // 模型偶尔会把「校正：」一起输出
  for (const wchar_t* label : {L"校正：", L"校正:"}) {
    const size_t pos = s.find(label);
    if (pos != std::wstring::npos)
      s = s.substr(pos + wcslen(label));
  }
  const size_t b = s.find_first_not_of(kTrimChars);
  s = b == std::wstring::npos ? std::wstring() : s.substr(b, s.find_last_not_of(kTrimChars) - b + 1);
  if (s.empty() || s == draft)
    return L"";
  if (s.size() > draft.size() + 2 || s.size() + 2 < draft.size())
    return L"";
  return s;
}

void RimeWithWeaselHandler::_TriggerLLMPrediction(WeaselSessionId ipc_id,
                                                  const std::wstring& current_input,
                                                  DWORD delay_ms,
                                                  const std::wstring& completion_prefix,
                                                  const std::wstring& zhuyin,
                                                  bool complete) {
  const bool llm_available = m_llm_provider && m_llm_provider->IsAvailable();
  const bool correct = _TypoLLMAvailable() && !zhuyin.empty() && !completion_prefix.empty();
  if (!llm_available && !m_personal && !correct) {
    LOG(WARNING) << "[LLM] neither LLM provider nor personal lexicon is available";
    return;
  }

  // 个人词库（查内存，不到 1ms）：输入中补全 → 以 Rime 转换结果开头的常用词；
  // 提交后 → 当前窗口最后一个词之后最常接的词
  std::vector<std::wstring> personal;
  if (m_personal && complete) {
    personal = completion_prefix.empty()
                   ? m_personal->NextAfter(m_context_history ? m_context_history->GetActiveKey()
                                                             : std::wstring(),
                                           m_personal_max)
                   : m_personal->CompleteFrom(completion_prefix, m_personal_max);
    // 补全候选若和目前的字完全相同，没有意义
    personal.erase(std::remove(personal.begin(), personal.end(), completion_prefix),
                   personal.end());
  }
  if (!llm_available && personal.empty() && !correct)
    return;

  // LLM 上下文：当前窗口最近的前文 + 输入中补全时 Rime 当前的转换结果
  std::wstring history;
  if (m_context_history)
    history = m_context_history->GetRecentContext(m_llm_context_max_chars);
  history += get_session_status(ipc_id).mixed_text;  // 中英混打中尚未送出的部分也是前文
  const std::wstring context = history + completion_prefix;

  if (m_dev_console && m_dev_console->IsEnabled()) {
    m_dev_console->WriteLine(L"[LLM] ========== 开始预测 ==========");
    m_dev_console->WriteLine(L"[LLM] 上下文: " + context);
    std::wstring joined;
    for (const auto& p : personal)
      joined += (joined.empty() ? L"" : L" / ") + p;
    m_dev_console->WriteLine(L"[LLM] 个人词库候选: " + (joined.empty() ? L"(无)" : joined));
  }

  // 生成新的请求序号，用于标记“最新一次”预测请求
  const uint64_t request_seq = ++m_llm_request_seq;

  std::wstring context_copy = context;
  std::wstring current_input_copy = current_input;
  std::wstring prefix_copy = completion_prefix;
  std::wstring typo_prompt;
  {
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    typo_prompt = m_typo_prompt;
  }
  // 校正只看组字区的内容：组字区里已确定的部分（混打、标点）当前文，不带之前送出的文字
  const std::wstring typo_context = get_session_status(ipc_id).mixed_text;

  std::thread([this, ipc_id, request_seq, context_copy, current_input_copy, delay_ms,
               prefix_copy, personal, llm_available, history, zhuyin, correct, complete,
               typo_prompt, typo_context]() {
    // 生成途中又有新请求（继续打字）：本机模型立即停止，让新的请求接着开始
    LLMCancelScope cancel_scope(
        [this, request_seq] { return request_seq != m_llm_request_seq.load(); });
    // 防抖：等待期间若又有新请求（例如继续打字），直接放弃，不占用 GPU
    if (delay_ms > 0) {
      Sleep(delay_ms);
      if (request_seq != m_llm_request_seq.load())
        return;
    }

    // 写入候选并刷新候选窗。在锁内检查是否已有更新的请求（或已被取消），是则丢弃。
    // 刷新 UI 必须在服务端的 IPC 锁下进行：按键处理线程同时在用 librime 与候选窗（Direct2D）。
    // corrections：开头几个候选是整句校正（候选窗标示「校正」）
    auto publish = [&](std::vector<std::wstring> candidates, size_t corrections) {
      {
        std::lock_guard<std::mutex> lock(m_llm_mutex);
        if (request_seq != m_llm_request_seq.load())
          return false;
        m_current_llm_candidates = std::move(candidates);
        m_llm_correction_count = corrections;
      }
      std::lock_guard<std::mutex> api_lock(weasel::ServerApiMutex());
      if (request_seq == m_llm_request_seq.load())
        _UpdateUI(ipc_id);
      return true;
    };
    // 依序合并、去重，最多 5 个（对应 Tab、Shift+2~5）
    auto merge = [](std::vector<std::wstring> merged, const std::vector<std::wstring>& more) {
      for (const auto& c : more) {
        if (merged.size() >= 5)
          break;
        if (std::find(merged.begin(), merged.end(), c) == merged.end())
          merged.push_back(c);
      }
      return merged;
    };

    // 1) 先显示个人词库的候选（几乎零延迟）
    if (!personal.empty() && !publish(personal, 0))
      return;
    if (!llm_available && !correct)
      return;

    // 2) 注音整句校正：LLM 依前文与注音推测真正要打的句子，放在第一个；之后的续写接在校正结果后面
    std::vector<std::wstring> corrections;
    std::wstring context = context_copy;
    std::wstring prefix = prefix_copy;
    if (correct) {
      std::wstring corrected;
      {
        std::lock_guard<std::mutex> infer_lock(m_llm_infer_mutex);
        if (request_seq != m_llm_request_seq.load() || !m_typo_llm)
          return;
        corrected = CleanCorrection(
            m_typo_llm->CorrectSentence(typo_context, zhuyin, prefix_copy, typo_prompt),
            prefix_copy);
      }
      if (m_dev_console && m_dev_console->IsEnabled())
        m_dev_console->WriteLine(L"[LLM] 整句校正: " + prefix_copy + L" → " +
                                 (corrected.empty() ? L"(不需校正)" : corrected));
      if (!corrected.empty()) {
        corrections.push_back(corrected);
        if (!publish(merge(corrections, personal), corrections.size()))
          return;
        context = history + corrected;
        prefix = corrected;
      }
    }
    if (!complete || !llm_available)
      return;

    // 3) 再用 LLM 补足：同一时间只能有一个推理（llama.cpp context 非线程安全），
    //    排到时若已有更新的请求就放弃
    std::vector<std::wstring> candidates;
    {
      std::lock_guard<std::mutex> infer_lock(m_llm_infer_mutex);
      if (request_seq != m_llm_request_seq.load() || !m_llm_provider)
        return;
      candidates = m_llm_provider->PredictCandidates(context, current_input_copy, 5);
    }
    candidates = CleanLLMCandidates(candidates, prefix);

    // 校正排最前，接着个人词库，LLM 续写接在后面
    std::vector<std::wstring> merged = merge(merge(corrections, personal), candidates);
    if (m_dev_console && m_dev_console->IsEnabled())
      m_dev_console->WriteLine(L"[LLM] 预测完成，共 " + std::to_wstring(merged.size()) +
                               L" 个候选（个人词库 " + std::to_wstring(personal.size()) + L"）");
    publish(std::move(merged), corrections.size());
  }).detach();
}

void RimeWithWeaselHandler::_ExitLLMPredictionMode(WeaselSessionId ipc_id) {
  m_llm_prediction_mode = false;
  m_llm_completion_active = false;
  {
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    ++m_llm_request_seq;  // 丢弃仍在进行中的预测
    m_current_llm_candidates.clear();
  }
  
  // 强制隐藏候选栏
  if (m_ui) {
    m_ui->Hide();
    if (m_dev_console && m_dev_console->IsEnabled()) {
      m_dev_console->WriteLine(L"[LLM] 强制隐藏候选栏");
    }
  }
  
  _UpdateUI(ipc_id);

  if (m_dev_console && m_dev_console->IsEnabled()) {
    m_dev_console->WriteLine(L"[LLM] 退出LLM预测模式");
  }
}

void RimeWithWeaselHandler::_UpdateContextKey(WeaselSessionId ipc_id) {
  if (!m_context_history)
    return;

  // 应用名（Rime session 的 client_app）
  std::wstring app;
  char app_name[256] = {0};
  if (rime_api->get_property(to_session_id(ipc_id), "client_app", app_name,
                             sizeof(app_name) - 1))
    app = u8tow(app_name);

  // 前景窗口（顶层窗口）与标题：同一应用的不同窗口、浏览器不同分页、不同聊天室各自一份前文。
  // 标题去掉数字与 * ● 等，避免未读数、「已修改」标记变化时被当成另一个窗口。
  HWND hwnd = GetForegroundWindow();
  if (hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root)
      hwnd = root;
  }
  // 不能用 GetWindowTextW：它會對視窗送 WM_GETTEXT 並等回覆，視窗忙碌或正在等輸入法時
  // 就卡住整個服務（所有應用程式跟著凍結）。InternalGetWindowText 直接讀系統存的標題，不送訊息
  wchar_t title[256] = {0};
  if (hwnd)
    InternalGetWindowText(hwnd, title, (int)(sizeof(title) / sizeof(title[0])) - 1);
  std::wstring title_key;
  for (const wchar_t* p = title; *p; ++p) {
    if (iswdigit(*p) || *p == L'*' || *p == L'●' || *p == L'•')
      continue;
    title_key += *p;
  }
  wchar_t hwnd_buf[32] = {0};
  swprintf_s(hwnd_buf, L"%p", (void*)hwnd);

  m_context_history->SetIdleTimeout(m_llm_context_idle_minutes * 60ull * 1000ull);
  m_context_history->SetActiveKey(app + L"|" + hwnd_buf + L"|" + title_key, m_dev_console);
}

bool RimeWithWeaselHandler::_TypoLLMAvailable() const {
  return m_typo_llm_on && m_typo_llm && m_typo_llm->IsAvailable();
}

void RimeWithWeaselHandler::_LoadTypoProvider(RimeConfig* config) {
  m_typo_llm = nullptr;
  m_typo_owned.reset();
  if (!m_typo_llm_on)
    return;
  auto read = [&](const char* key) {
    char value[4096] = {0};
    return rime_api->config_get_string(config, key, value, sizeof(value) - 1) ? std::string(value)
                                                                               : std::string();
  };
  // 校正使用的模型由设定程式从「语言模型」清单选用后展开到 llm/typo/*
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
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    m_typo_prompt = u8tow(read("llm/typo/prompt"));  // 自訂校正指令，空字串用預設
  }
  const bool no_think = read("llm/typo/disable_thinking") == "true";
  int think_tokens = 2048;  // 開啟思考時的思考長度上限（0 = 不限制）
  if (rime_api->config_get_int(config, "llm/typo/think_tokens", &think_tokens))
    think_tokens = (std::max)(0, think_tokens);
  else
    think_tokens = 2048;

  // 和智慧预测是同一个模型：共用，避免同一个模型载入两次
  if (m_llm_provider && m_llm_provider->IsAvailable()) {
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
      m_typo_llm = m_llm_provider.get();
      LOG(INFO) << "Typo correction shares the prediction model.";
      return;
    }
  }
  if (type == "llamacpp" && !model_path.empty()) {
    LLMLocalModelSpec spec;
    spec.model_path = model_path;
    spec.instruct = model_type != "base";
    // 前文 + 一句话 2048 就够；开启思考时再加上思考额度（不限制时给 8192）
    spec.n_ctx = no_think || model_type == "base" ? 2048
                 : think_tokens <= 0              ? 8192
                                                  : (std::min)(2048 + think_tokens, 32768);
    spec.disable_thinking = no_think;
    spec.think_tokens = think_tokens;
    int value = 0;
    if (rime_api->config_get_int(config, "llm/llamacpp/n_gpu_layers", &value))
      spec.n_gpu_layers = value;
    if (rime_api->config_get_int(config, "llm/llamacpp/n_threads", &value) && value > 0)
      spec.n_threads = value;
    auto provider = std::make_unique<LlamaCppProvider>();
    if (provider->LoadModelDirect(spec, 0.0)) {
      provider->SetPromptPrefix(u8tow(prompt));
      m_typo_owned = std::move(provider);
    } else {
      LOG(ERROR) << "Typo correction: failed to load model " << model_path;
    }
  } else if (type == "openai" && !api_url.empty()) {
    auto provider = std::make_unique<OpenAICompatibleProvider>();
    provider->ConfigureDirect(api_url, api_key, model, u8tow(prompt), no_think, think_tokens);
    m_typo_owned = std::move(provider);
  } else {
    LOG(WARNING) << "Typo correction: no model selected (llm/typo/type)";
  }
  m_typo_llm = m_typo_owned.get();
}

bool RimeWithWeaselHandler::_PredictionAvailable() const {
  if (!m_llm_enabled)
    return false;
  return (m_llm_provider && m_llm_provider->IsAvailable()) || m_personal != nullptr;
}

void RimeWithWeaselHandler::_ScheduleLLMCompletion(WeaselSessionId ipc_id, DWORD delay_ms) {
  const bool predict = _PredictionAvailable();
  if (!predict && !_TypoLLMAvailable())
    return;

  // Rime 若此刻提交会得到的文字（整句转换）；没有时退回第一个候选
  std::wstring preview;
  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(to_session_id(ipc_id), &ctx)) {
    if (ctx.commit_text_preview && *ctx.commit_text_preview)
      preview = u8tow(ctx.commit_text_preview);
    else if (ctx.menu.num_candidates > 0 && ctx.menu.candidates[0].text)
      preview = u8tow(ctx.menu.candidates[0].text);
    rime_api->free_context(&ctx);
  }
  // 注音：往回选字时转换结果后面会接着还没转换的按键，只把光标前转好的中文当前文
  {
    char schema_id[256] = {0};
    rime_api->get_current_schema(to_session_id(ipc_id), schema_id, sizeof(schema_id));
    if (strncmp(schema_id, "bopomofo", 8) == 0) {
      while (!preview.empty() && preview.back() < 0x80)
        preview.pop_back();
    }
  }
  if (preview.empty()) {
    _CancelLLMCompletion();
    return;
  }

  // 续写：开启智慧预测且（输入中补全或按 ` 键手动触发，delay_ms 为 0）；
  // 整句校正：开启 LLM 整句校正（与智慧预测、Rime 容错各自独立）
  const bool complete = predict && (m_llm_while_typing || delay_ms == 0);
  const std::wstring zhuyin = _TypoLLMAvailable() ? _ComposingZhuyin(ipc_id) : std::wstring();
  if (!complete && zhuyin.empty()) {
    _CancelLLMCompletion();
    return;
  }

  // 旧候选（上一个键的补全或提交后的下一词预测）已不适用，先清掉
  {
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    m_current_llm_candidates.clear();
  }
  m_llm_prediction_mode = true;
  m_llm_completion_active = true;
  if (m_dev_console && m_dev_console->IsEnabled()) {
    m_dev_console->WriteLine(L"[LLM] 输入中补全（" + std::to_wstring(delay_ms) +
                             L"ms 后）: " + preview + (zhuyin.empty() ? L"" : L"，注音: " + zhuyin));
  }
  _TriggerLLMPrediction(ipc_id, L"", delay_ms, preview, zhuyin, complete);
}

std::wstring RimeWithWeaselHandler::_ComposingZhuyin(WeaselSessionId ipc_id) {
  const RimeSessionId session_id = to_session_id(ipc_id);
  bool zhuyin_schema = false;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    zhuyin_schema = status.schema_id && strncmp(status.schema_id, "bopomofo", 8) == 0;
    rime_api->free_status(&status);
  }
  const char* raw = zhuyin_schema ? rime_api->get_input(session_id) : nullptr;
  if (!raw || !*raw)
    return L"";
  // 往回选字时只取光标前的部分（与送给模型的前文一致）；光标在最前面时 Rime 转换整段
  std::string before(raw);
  const size_t caret = rime_api->get_caret_pos(session_id);
  if (caret > 0 && caret < before.size())
    before.resize(caret);
  const char* input = before.c_str();
  // 注音方案（大千式）的按键 → 注音符号；声调之后断开，方便模型分辨音节
  static const char kKeys[] = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/-";
  static const wchar_t kSymbols[] = L"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ";
  static const char kToneKeys[] = "6347";
  static const wchar_t kTones[] = L"ˊˇˋ˙";
  std::wstring zhuyin;
  for (const char* p = input; *p; ++p) {
    if (const char* k = strchr(kKeys, *p)) {
      zhuyin += kSymbols[k - kKeys];
    } else if (const char* t = strchr(kToneKeys, *p)) {
      zhuyin += kTones[t - kToneKeys];
      zhuyin += L' ';
    } else if (*p == ' ' || *p == '\'') {
      if (!zhuyin.empty() && zhuyin.back() != L' ')
        zhuyin += L' ';
    } else {
      return L"";  // 不是大千键位（例如其他注音键盘布局），不校正
    }
  }
  while (!zhuyin.empty() && zhuyin.back() == L' ')
    zhuyin.pop_back();
  return zhuyin;
}

void RimeWithWeaselHandler::_CancelLLMCompletion() {
  if (!m_llm_completion_active)
    return;
  m_llm_completion_active = false;
  m_llm_prediction_mode = false;
  std::lock_guard<std::mutex> lock(m_llm_mutex);
  ++m_llm_request_seq;  // 让尚未完成的补全请求作废
  m_current_llm_candidates.clear();
}

bool RimeWithWeaselHandler::_CommitLLMCandidate(WeaselSessionId ipc_id,
                                                size_t llm_index,
                                                EatLine eat) {
  std::wstring selected;
  {
    std::lock_guard<std::mutex> lock(m_llm_mutex);
    if (llm_index >= m_current_llm_candidates.size())
      return false;
    selected = m_current_llm_candidates[llm_index];
    m_current_llm_candidates.clear();
  }
  if (m_dev_console && m_dev_console->IsEnabled()) {
    m_dev_console->WriteLine(L"[LLM] 选择LLM候选词: " + std::to_wstring(llm_index + 1) +
                             L". " + selected);
  }

  // 往回选字（光标不在最后）：候选取代光标前的部分，留在组字区；光标后的按键留给 Rime 继续编辑
  {
    const RimeSessionId session_id = to_session_id(ipc_id);
    const char* raw = rime_api->get_input(session_id);
    const std::string input = raw ? raw : "";
    const size_t caret = rime_api->get_caret_pos(session_id);
    char schema_id[256] = {0};
    rime_api->get_current_schema(session_id, schema_id, sizeof(schema_id));
    if (strncmp(schema_id, "bopomofo", 8) == 0 && caret > 0 && caret < input.size()) {
      SessionStatus& ss = get_session_status(ipc_id);
      ss.mixed_text += selected;
      rime_api->clear_composition(session_id);
      rime_api->set_input(session_id, input.substr(caret).c_str());
      m_llm_completion_active = false;
      m_llm_prediction_mode = false;
      _Respond(ipc_id, eat);
      _UpdateUI(ipc_id);
      return true;
    }
  }

  // 补全候选已包含 Rime 的转换结果，丢弃 composition 后整段提交
  rime_api->clear_composition(to_session_id(ipc_id));
  if (m_context_history)
    m_context_history->AddText(selected, m_dev_console);
  if (m_personal)
    m_personal->Record(m_context_history ? m_context_history->GetActiveKey() : L"", selected);
  m_pending_llm_commit = selected;
  m_llm_completion_active = false;

  // 继续预测下一个词（可用 llm/predict_after_commit 关闭；关闭智慧预测时只有整句校正，不接着预测）
  const bool predict_next = m_llm_after_commit && _PredictionAvailable();
  m_llm_prediction_mode = predict_next;
  if (predict_next)
    _TriggerLLMPrediction(ipc_id);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return true;
}
