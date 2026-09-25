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
  # loaded by running apps: rename aside, then copy (new processes pick up the new file)
  Remove-Item "$($p.dst).old" -Force -ErrorAction SilentlyContinue
  Move-Item $p.dst "$($p.dst).old" -Force
  Copy-Item $p.src $p.dst -Force
}
"done" | Out-File "$root\install_tsf_system.done"
Stop-Transcript
