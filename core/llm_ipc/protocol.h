#pragma once

// 輸入法與 LLM 推理行程之間的訊息格式（各平台共用）。
// 每則訊息：u32 長度 | u8 種類 | u32 編號 | 內容；數字一律 little-endian，字串是 UTF-8。
// 輸入法送出請求，推理行程依序處理、以同一個編號回覆；取消與記錄訊息不需要回覆。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../platform/process.h"

namespace llm_ipc {

enum class Op : uint8_t {
  // 設定（推理行程重新啟動時會依序重送）
  kCreate = 1,           // 種類（llamacpp / openai / hf_constraint / session） → 成功與否
  kLoadConfig = 2,       // 設定檔名 → 成功與否
  kLoadModelDirect = 3,  // 模型規格、temperature → 成功與否
  kConfigureDirect = 4,  // api_url、api_key、model、prompt、關閉思考、思考上限
  kSetPromptPrefix = 5,  // prompt
  // 推理
  kPredict = 10,      // 前文、目前輸入、數量 → 候選清單
  kCorrect = 11,      // 前文、注音、初稿、指令 → 句子
  kScore = 12,        // 前文、文字、要不要逐字 → 成功與否、總分、逐字分數
  kIsAvailable = 13,  // → 可用與否
  kSessionOpen = 14,  // 模型規格 → 成功與否、錯誤訊息
  kSessionChat = 15,  // system、user、max_tokens → 成功與否、輸出、錯誤訊息
  // 借用已載入的本機模型對話（模型不同或放不下時不做）：
  // 模型路徑、instruct、system、user、max_tokens → 結果（0 成功、1 不適用、2 失敗）、輸出、錯誤訊息
  kChat = 16,
  kScoreBatch = 17,  // 前文、候選清單 → 每個候選的總分（評不了的是 NaN）
  // 不需回覆
  kCancel = 100,  // 取消編號為 id 的請求
  // 推理行程送出
  kReply = 200,  // 回覆編號為 id 的請求
  kLog = 201,    // 開發終端的一行記錄
};

class Writer {
 public:
  Writer(Op op, uint32_t id) {
    buf_.resize(4);  // 長度，Finish 時填
    U8((uint8_t)op);
    U32(id);
  }
  void U8(uint8_t v) { buf_.push_back((char)v); }
  void Flag(bool v) { U8(v ? 1 : 0); }
  void U32(uint32_t v) { Raw(&v, sizeof(v)); }
  void I32(int32_t v) { Raw(&v, sizeof(v)); }
  void F64(double v) { Raw(&v, sizeof(v)); }
  void Str(const std::string& s) {
    U32((uint32_t)s.size());
    buf_.append(s);
  }
  void StrList(const std::vector<std::string>& list) {
    U32((uint32_t)list.size());
    for (const auto& s : list)
      Str(s);
  }
  void F64List(const std::vector<double>& list) {
    U32((uint32_t)list.size());
    for (double v : list)
      F64(v);
  }
  // 改編號（重送設定時用）
  void SetId(uint32_t id) { std::memcpy(&buf_[5], &id, sizeof(id)); }
  // 填好長度，回傳可以直接寫進管道的位元組
  const std::string& Finish() {
    const uint32_t size = (uint32_t)(buf_.size() - 4);
    std::memcpy(&buf_[0], &size, sizeof(size));
    return buf_;
  }

 private:
  void Raw(const void* p, size_t n) { buf_.append(static_cast<const char*>(p), n); }
  std::string buf_;
};

// 讀取時任何欄位超出範圍都會讓 Ok() 變成 false，之後讀到的都是預設值
class Reader {
 public:
  explicit Reader(std::string payload) : s_(std::move(payload)) {
    op_ = (Op)U8();
    id_ = U32();
  }
  Op op() const { return op_; }
  uint32_t id() const { return id_; }
  bool Ok() const { return ok_; }

  uint8_t U8() {
    uint8_t v = 0;
    Raw(&v, sizeof(v));
    return v;
  }
  bool Flag() { return U8() != 0; }
  uint32_t U32() {
    uint32_t v = 0;
    Raw(&v, sizeof(v));
    return v;
  }
  int32_t I32() {
    int32_t v = 0;
    Raw(&v, sizeof(v));
    return v;
  }
  double F64() {
    double v = 0;
    Raw(&v, sizeof(v));
    return v;
  }
  std::string Str() {
    const uint32_t n = U32();
    if (!ok_ || n > s_.size() - pos_) {
      ok_ = false;
      return std::string();
    }
    std::string v = s_.substr(pos_, n);
    pos_ += n;
    return v;
  }
  std::vector<std::string> StrList() {
    std::vector<std::string> list;
    const uint32_t n = U32();
    for (uint32_t i = 0; ok_ && i < n; ++i)
      list.push_back(Str());
    return list;
  }
  std::vector<double> F64List() {
    std::vector<double> list;
    const uint32_t n = U32();
    for (uint32_t i = 0; ok_ && i < n; ++i)
      list.push_back(F64());
    return list;
  }

 private:
  void Raw(void* p, size_t n) {
    if (!ok_ || n > s_.size() - pos_) {
      ok_ = false;
      return;
    }
    std::memcpy(p, s_.data() + pos_, n);
    pos_ += n;
  }
  std::string s_;
  size_t pos_ = 0;
  bool ok_ = true;
  Op op_;
  uint32_t id_;
};

// 單則訊息的上限；超過表示資料錯亂，直接斷線
constexpr uint32_t kMaxMessageSize = 64u << 20;

inline bool ReadMessage(platform::Pipe& pipe, std::string* payload) {
  uint32_t size = 0;
  if (!pipe.Read(&size, sizeof(size)) || size < 5 || size > kMaxMessageSize)
    return false;
  payload->resize(size);
  return pipe.Read(&(*payload)[0], size);
}

inline bool WriteMessage(platform::Pipe& pipe, Writer& message) {
  const std::string& bytes = message.Finish();
  return pipe.Write(bytes.data(), bytes.size());
}

// 本機模型規格（LLMLocalModelSpec）的欄位順序
struct ModelSpecFields {
  std::string model_path;
  bool instruct = true;
  int32_t n_ctx = 8192;
  int32_t n_gpu_layers = 0;
  int32_t n_threads = 4;
  bool disable_thinking = false;
  int32_t think_tokens = 2048;

  void Write(Writer& w) const {
    w.Str(model_path);
    w.Flag(instruct);
    w.I32(n_ctx);
    w.I32(n_gpu_layers);
    w.I32(n_threads);
    w.Flag(disable_thinking);
    w.I32(think_tokens);
  }
  void Read(Reader& r) {
    model_path = r.Str();
    instruct = r.Flag();
    n_ctx = r.I32();
    n_gpu_layers = r.I32();
    n_threads = r.I32();
    disable_thinking = r.Flag();
    think_tokens = r.I32();
  }
};

}  // namespace llm_ipc
