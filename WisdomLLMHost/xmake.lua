target("WisdomLLMHost")
  set_kind("binary")
  add_files("./*.cpp")
  add_files("$(projectdir)/core/llm/LLMProvider.cpp",
            "$(projectdir)/core/llm/LlamaCppProvider.cpp",
            "$(projectdir)/core/llm/HFConstraintProvider.cpp",
            "$(projectdir)/RimeWithWeasel/WeaselUtility.cpp",
            "$(projectdir)/core/llm_ipc/host.cpp",
            "$(projectdir)/core/net/win/http_win.cpp",
            "$(projectdir)/core/platform/win/process_win.cpp")
  add_includedirs("$(projectdir)/llamatest/llama.cpp/include", "$(projectdir)/llamatest/ggml/include")
  add_rules("use_weaselconstants")
  add_links("rime", "llama", "ggml", "ggml-base")
  after_build(function(target)
    os.cp(path.join(target:targetdir(), "WisdomLLMHost.exe"), "$(projectdir)/output")
  end)
