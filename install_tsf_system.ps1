# Replace the registered TSF DLLs (System32 / SysWOW64 copies made by WeaselSetup) with the hardened build. Run elevated.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Start-Transcript "$root\install_tsf_system.log" -Force
$backup = 'C:\Program Files\Rime\wisdom-weasel\backup-b8184\system'
New-Item -ItemType Directory -Force $backup | Out-Null

$pairs = @(
  @{ src = "$root\output\weaselx64.dll"; dst = "$env:WINDIR\System32\weasel.dll"; bak = "System32-weasel.dll" },
  @{ src = "$root\output\weasel.dll";    dst = "$env:WINDIR\SysWOW64\weasel.dll"; bak = "SysWOW64-weasel.dll" }
)
foreach ($p in $pairs) {
  if (-not (Test-Path "$backup\$($p.bak)")) { Copy-Item $p.dst "$backup\$($p.bak)" }
  # loaded by running apps: rename aside, then copy (new processes pick up the new file).
  # An older .old may still be loaded too: clean up what we can, and use a fresh name if it stays locked
  Get-ChildItem "$($p.dst).old*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
  $aside = "$($p.dst).old"
  if (Test-Path $aside) { $aside = "$($p.dst).old-$(Get-Date -Format yyyyMMddHHmmss)" }
  Move-Item $p.dst $aside -Force
  Copy-Item $p.src $p.dst -Force
}
"done" | Out-File "$root\install_tsf_system.done"
Stop-Transcript
