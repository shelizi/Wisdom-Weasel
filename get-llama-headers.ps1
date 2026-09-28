# Downloads the llama.cpp headers used by the LLM host (WisdomLLMHost) into llamatest\.
# The import libraries (llama.lib, ggml.lib, ggml-base.lib) for the same version are in lib64\.
param(
  [string]$Version = 'b11177',
  [string]$Dest = (Join-Path $PSScriptRoot 'llamatest')
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Windows PowerShell 5.1 downloads very slowly with the progress bar
if (Test-Path (Join-Path $Dest 'llama.cpp\include\llama.h')) {
  Write-Host "llama.cpp headers already in $Dest"
  exit 0
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "llama-headers-$Version"
$archive = "$tmp.tar.gz"
Invoke-WebRequest "https://github.com/ggml-org/llama.cpp/archive/refs/tags/$Version.tar.gz" -OutFile $archive
New-Item -ItemType Directory -Force $tmp | Out-Null
# Windows' own bsdtar: Git's GNU tar reads "C:" in a path as a remote host
$tar = Join-Path $env:SystemRoot 'System32/tar.exe'
& $tar -xzf $archive -C $tmp "llama.cpp-$Version/include" "llama.cpp-$Version/ggml/include"
if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
New-Item -ItemType Directory -Force (Join-Path $Dest 'llama.cpp'), (Join-Path $Dest 'ggml') | Out-Null
Copy-Item (Join-Path $tmp "llama.cpp-$Version/include") (Join-Path $Dest 'llama.cpp') -Recurse -Force
Copy-Item (Join-Path $tmp "llama.cpp-$Version/ggml/include") (Join-Path $Dest 'ggml') -Recurse -Force
Remove-Item $tmp, $archive -Recurse -Force
Write-Host "llama.cpp $Version headers in $Dest"
