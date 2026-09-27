target("RimeWithWeasel")
  set_kind("static")
  add_files("./*.cpp")
  -- 跨平台核心：個人資料加密
  add_files("$(projectdir)/core/crypto/*.cpp", "$(projectdir)/core/platform/win/*.cpp")
  add_files("$(projectdir)/core/third_party/monocypher/monocypher.c")
  -- 跨平台核心：輸入法邏輯（文字規則、推薦、選字統計、Rime 小工具）
  add_files("$(projectdir)/core/ime/*.cpp")
  add_rules("use_weaselconstants")
