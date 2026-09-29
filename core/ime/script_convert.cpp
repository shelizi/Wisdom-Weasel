#include "script_convert.h"

#include <logging.h>

#define Opencc_BUILT_AS_STATIC
#include <opencc/opencc.h>

#include "../base/utf8.h"

namespace ime {

namespace {

bool Valid(void* converter) {
  return converter && converter != (void*)-1;
}

}  // namespace

ScriptConverter::~ScriptConverter() {
  Close();
}

void ScriptConverter::Close() {
  if (Valid(converter_))
    opencc_close(converter_);
  converter_ = nullptr;
  config_.clear();
}

bool ScriptConverter::Load(const std::filesystem::path& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (Valid(converter_) && config_ == config)
    return true;
  Close();
#ifdef _MSC_VER
  void* converter = opencc_open_w(config.wstring().c_str());
#else
  void* converter = opencc_open(config.u8string().c_str());
#endif
  if (!Valid(converter)) {
    LOG(ERROR) << "OpenCC: cannot open " << config.u8string() << ": " << opencc_error();
    return false;
  }
  converter_ = converter;
  config_ = config;
  return true;
}

bool ScriptConverter::Loaded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Valid(converter_);
}

std::wstring ScriptConverter::Convert(const std::wstring& text) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Valid(converter_) || text.empty())
    return text;
  const std::string input = utf8::FromWide(text);
  char* output = opencc_convert_utf8(converter_, input.c_str(), input.size());
  if (!output)
    return text;
  std::wstring result = utf8::ToWide(output);
  opencc_convert_utf8_free(output);
  return result;
}

}  // namespace ime
