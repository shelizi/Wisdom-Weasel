target("WeaselServer")
  set_kind("binary")
  add_files("./*.cpp")
  -- 共用的 LLM 與個人詞彙程式碼；本機模型與 HF 的 provider 在推理行程（WisdomLLMHost）裡編譯
  add_files("$(projectdir)/core/llm/LLMProvider.cpp", "$(projectdir)/core/llm/RemoteLLMProvider.cpp",
            "$(projectdir)/core/llm/ContextHistory.cpp", "$(projectdir)/core/llm/MemoryCompressor.cpp",
            "$(projectdir)/core/personal/PersonalLexicon.cpp", "$(projectdir)/core/personal/PersonalRefiner.cpp",
            "$(projectdir)/core/personal/LearnFilter.cpp")
  add_files("$(projectdir)/core/llm_ipc/client.cpp", "$(projectdir)/core/platform/win/process_win.cpp",
            "$(projectdir)/core/net/win/http_win.cpp")
  add_rules("add_rcfiles", "subwin")
  add_links("imm32", "kernel32", "rime")
  add_deps("WeaselUI", "WeaselIPC", "RimeWithWeasel", "WeaselIPCServer")

  add_files("$(projectdir)/PerMonitorHighDPIAware.manifest")
  add_ldflags("/DEBUG /OPT:REF /OPT:ICF /LARGEADDRESSAWARE /ERRORREPORT:QUEUE")
  set_policy("windows.manifest.uac", "invoker")
  before_build(function(target)
    local target_dir = path.join(target:targetdir(), target:name())
    if not os.exists(target_dir) then
      os.mkdir(target_dir)
    end
    target:set("targetdir", target_dir)
  end)
  after_build(function(target)
    if is_arch("x86") then
      os.cp(path.join(target:targetdir(), "WeaselServer.exe"), "$(projectdir)/output/Win32")
      os.cp(path.join(target:targetdir(), "WeaselServer.pdb"), "$(projectdir)/output/Win32")
    else
      os.cp(path.join(target:targetdir(), "WeaselServer.exe"), "$(projectdir)/output")
      os.cp(path.join(target:targetdir(), "WeaselServer.pdb"), "$(projectdir)/output")
    end
  end)

