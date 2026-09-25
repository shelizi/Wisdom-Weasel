#pragma once
#include <ResponseParser.h>
#include <functional>

namespace weasel {

template <typename T>
void TryDeserialize(boost::archive::text_wiarchive& ia, T& t) {
  try {
    ia >> t;
  } catch (const boost::archive::archive_exception& e) {
    // 运行在宿主应用进程内：不弹 MessageBox（会阻塞/打断宿主应用），只记录调试输出
    const std::string msg =
        std::string("[weasel] boost::archive::archive_exception: ") + e.what() + "\n";
    OutputDebugStringA(msg.c_str());
  }
}
class Deserializer {
 public:
  typedef std::vector<std::wstring> KeyType;
  typedef std::shared_ptr<Deserializer> Ptr;
  typedef std::function<Ptr(ResponseParser* pTarget)> Factory;

  Deserializer(ResponseParser* pTarget) : m_pTarget(pTarget) {}
  virtual ~Deserializer() {}
  virtual void Store(KeyType const& key, std::wstring const& value) {}

  static void Initialize(ResponseParser* pTarget);
  static void Define(std::wstring const& action, Factory factory);
  static bool Require(std::wstring const& action, ResponseParser* pTarget);

 protected:
  ResponseParser* m_pTarget;

 private:
  static std::map<std::wstring, Factory> s_factories;
};

}  // namespace weasel
