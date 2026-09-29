# Extracts OpenCC (headers + static libs, with marisa) from the rime-deps archives into deps\opencc.
# Used to convert committed text to Simplified Chinese (llm/simplified); the dictionaries come from Rime's data\opencc.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dest = Join-Path $root 'deps\opencc'
if (Test-Path (Join-Path $dest 'include\opencc\opencc.h')) {
  Write-Host "OpenCC already in $dest"
  exit 0
}
$7z = if (Test-Path (Join-Path $root '7z.exe')) { Join-Path $root '7z.exe' } else { '7z' }
foreach ($arch in 'x64', 'x86') {
  $archive = Get-ChildItem $root -Filter "rime-deps-*-Windows-msvc-$arch.7z" | Select-Object -First 1
  if (-not $archive) { throw "rime-deps-*-Windows-msvc-$arch.7z not found (run get-rime.ps1 first)" }
  $tmp = Join-Path ([IO.Path]::GetTempPath()) "opencc-$arch"
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  & $7z x $archive.FullName "-o$tmp" 'include\opencc\*' 'lib\opencc.lib' 'lib\marisa.lib' -y | Out-Null
  if ($LASTEXITCODE) { throw "7z failed on $($archive.Name)" }
  $libDir = Join-Path $dest $(if ($arch -eq 'x64') { 'x64' } else { 'Win32' })
  New-Item -ItemType Directory -Force $libDir | Out-Null
  Copy-Item (Join-Path $tmp 'lib\*.lib') $libDir
  if ($arch -eq 'x64') {
    New-Item -ItemType Directory -Force (Join-Path $dest 'include') | Out-Null
    Copy-Item (Join-Path $tmp 'include\opencc') (Join-Path $dest 'include') -Recurse -Force
  }
  Remove-Item $tmp -Recurse -Force
}
Write-Host "OpenCC installed to $dest"
