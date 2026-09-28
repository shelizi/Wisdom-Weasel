# Downloads the llama.cpp runtime DLLs that the LLM host (WisdomLLMHost.exe) loads into output\llama,
# where the installer picks them up. -Flavor cpu: the CPU build; -Flavor cuda: the CUDA build plus the
# CUDA runtime (cudart, cuBLAS) for NVIDIA GPUs.
param(
  [ValidateSet('cpu', 'cuda')][string]$Flavor = 'cpu',
  [string]$Version = 'b11177',
  [string]$Cuda = '12.4',
  [string]$Dest = (Join-Path $PSScriptRoot 'output/llama')
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Windows PowerShell 5.1 downloads very slowly with the progress bar

$base = "https://github.com/ggml-org/llama.cpp/releases/download/$Version"
if ($Flavor -eq 'cuda') {
  $zips = @("llama-$Version-bin-win-cuda-$Cuda-x64.zip", "cudart-llama-bin-win-cuda-$Cuda-x64.zip")
} else {
  $zips = @("llama-$Version-bin-win-cpu-x64.zip")
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "llama-runtime-$Version-$Flavor"
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $tmp | Out-Null
$extracted = Join-Path $tmp 'files'
foreach ($zip in $zips) {
  $file = Join-Path $tmp $zip
  Write-Host "Downloading $zip"
  Invoke-WebRequest "$base/$zip" -OutFile $file
  Expand-Archive $file $extracted -Force
}

# The DLLs the LLM host needs (the same list install_llama_update.ps1 installs); the command-line tools are left out
$wanted = @('llama.dll', 'ggml.dll', 'ggml-base.dll', 'ggml-rpc.dll', 'libomp.dll', 'ggml-cpu-*.dll',
            'ggml-cuda.dll', 'cudart64_*.dll', 'cublas64_*.dll', 'cublasLt64_*.dll')
Remove-Item $Dest -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $Dest -Force | Out-Null
Get-ChildItem $extracted -Recurse -File | Where-Object {
  $name = $_.Name
  $wanted | Where-Object { $name -like $_ }
} | Copy-Item -Destination $Dest

$required = @('llama.dll', 'ggml.dll', 'ggml-base.dll')
if ($Flavor -eq 'cuda') { $required += 'ggml-cuda.dll' }
foreach ($name in $required) {
  if (-not (Test-Path (Join-Path $Dest $name))) { throw "$name is missing from the llama.cpp $Version $Flavor release" }
}
Remove-Item $tmp -Recurse -Force
Write-Host "llama.cpp $Version ($Flavor): $((Get-ChildItem $Dest).Count) DLLs in $Dest"
