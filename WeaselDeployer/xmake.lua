target("WeaselDeployer")
  set_kind("binary")
  add_files("./*.cpp")
  add_rules("add_rcfiles", "use_weaselconstants", "subwin")
  add_links("imm32", "kernel32", "rime", "d2d1", "dwrite", "Shcore", "comdlg32")
  add_deps("WeaselIPC", "RimeWithWeasel")
  -- 網頁版設定：WebView2 SDK（get-webview2.ps1 下載到 deps\webview2）
  add_includedirs("$(projectdir)/deps/webview2/include")
  add_linkdirs("$(projectdir)/deps/webview2/$(arch)")
  add_links("WebView2LoaderStatic", "version")
  -- 設定後端（各平台共用）與 Windows 的 HTTP
  add_files("$(projectdir)/core/settings/*.cpp", "$(projectdir)/core/net/win/*.cpp")
  add_files("$(projectdir)/PerMonitorHighDPIAware.manifest")
  add_ldflags("/DEBUG /OPT:ICF /LARGEADDRESSAWARE /ERRORREPORT:QUEUE")
  before_build(function(target)
    local target_dir = path.join(target:targetdir(), target:name())
    if not os.exists(target_dir) then
      os.mkdir(target_dir)
    end
    target:set("targetdir", target_dir)
  end)
  after_build(function(target)
    if is_arch("x86") then
      os.cp(path.join(target:targetdir(), "WeaselDeployer.exe"), "$(projectdir)/output/Win32")
      os.cp(path.join(target:targetdir(), "WeaselDeployer.pdb"), "$(projectdir)/output/Win32")
    else
      os.cp(path.join(target:targetdir(), "WeaselDeployer.exe"), "$(projectdir)/output")
      os.cp(path.join(target:targetdir(), "WeaselDeployer.pdb"), "$(projectdir)/output")
    end
  end)
