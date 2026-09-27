# 設定程式的進入點開的是哪種視窗：不帶參數與 /dict 是網頁版（WisdomWebSettings），
# /legacy 是原本的設定視窗（#32770）。先跑過 run.ps1（沿用它建的沙盒）。
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$w = Join-Path $root 'msbuild\TestWebSettings'
if (-not (Test-Path "$w\bin\WeaselDeployer.exe")) { throw 'run run.ps1 first' }
Copy-Item (Join-Path $root 'output\WeaselDeployer.exe') "$w\bin" -Force
$env:WEASEL_TEST_USER_DIR = "$w\user"
$env:WISDOM_SETTINGS_WEB = Join-Path $root 'web\settings'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class EntryWin {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static string Find(uint pid) {
    string found = null;
    EnumWindows((h, p) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner == pid && IsWindowVisible(h)) {
        var c = new StringBuilder(256);
        GetClassName(h, c, 256);
        found = c.ToString(); return false;
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@
$expected = [ordered]@{ '' = 'WisdomWebSettings'; '/dict' = 'WisdomWebSettings'; '/legacy' = '#32770' }
$failed = 0
foreach ($arg in $expected.Keys) {
  # Windows PowerShell 5.1 不接受空的 ArgumentList
  $start = @{ FilePath = "$w\bin\WeaselDeployer.exe"; PassThru = $true }
  if ($arg) { $start.ArgumentList = $arg }
  $p = Start-Process @start
  $class = $null
  for ($i = 0; $i -lt 40 -and -not $class; ++$i) { Start-Sleep -Milliseconds 250; $class = [EntryWin]::Find([uint32]$p.Id) }
  $p.Kill()
  $p.WaitForExit()
  $name = if ($arg) { $arg } else { '(none)' }
  if ($class -eq $expected[$arg]) { "PASS $name -> $class" } else { "FAIL $name -> $class (expected $($expected[$arg]))"; ++$failed }
}
exit $failed
