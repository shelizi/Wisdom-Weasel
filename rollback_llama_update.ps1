# Restore files saved by install_llama_update.ps1. Run elevated.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Start-Transcript "$root\rollback_llama_update.log" -Force
$dest = 'C:\Program Files\Rime\wisdom-weasel'
$backup = "$dest\backup-b8184"
foreach ($f in Get-ChildItem $backup -File) {
  $target = "$dest\$($f.Name)"
  if (Test-Path $target) {
    Remove-Item "$target.new" -Force -ErrorAction SilentlyContinue
    Move-Item $target "$target.new" -Force
  }
  Copy-Item $f.FullName $target -Force
}
Get-Process WeaselServer -ErrorAction SilentlyContinue | Stop-Process -Force
"done" | Out-File "$root\rollback_llama_update.done"
Stop-Transcript
