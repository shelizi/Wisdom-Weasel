# Install locally rebuilt WeaselServer (llama.cpp b11177) into the Wisdom-Weasel install dir. Run elevated.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Start-Transcript "$root\install_llama_update.log" -Force
$dest = 'C:\Program Files\Rime\wisdom-weasel'
$llama = 'C:\Users\zex55\src\llama-b11177\bin'
$backup = "$dest\backup-b8184"

$files = @(
  @{ src = "$root\output\WeaselServer.exe"; name = 'WeaselServer.exe' },
  # LLM inference runs in its own process so a model crash never takes the IME down
  @{ src = "$root\output\WisdomLLMHost.exe"; name = 'WisdomLLMHost.exe' },
  @{ src = "$root\output\WeaselDeployer.exe"; name = 'WeaselDeployer.exe' },
  @{ src = "$root\output\rime.dll"; name = 'rime.dll' },
  # in-process TSF/IME modules (loaded into every app; hardened so IPC errors never crash the host)
  @{ src = "$root\output\weaselx64.dll"; name = 'weaselx64.dll' },
  @{ src = "$root\output\weasel.dll"; name = 'weasel.dll' },
  @{ src = "$root\output\weaselx64.ime"; name = 'weaselx64.ime' },
  @{ src = "$root\output\weasel.ime"; name = 'weasel.ime' }
)
$llamaDlls = @('llama.dll', 'ggml.dll', 'ggml-base.dll', 'ggml-cuda.dll', 'ggml-rpc.dll', 'libomp.dll',
               'cudart64_12.dll', 'cublas64_12.dll', 'cublasLt64_12.dll') +
             (Get-ChildItem $llama -Filter 'ggml-cpu-*.dll' | ForEach-Object Name)
$files += $llamaDlls | ForEach-Object { @{ src = "$llama\$_"; name = $_ } }

Get-Process WeaselServer, WisdomLLMHost -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2

# back up each original file once (never overwrite an existing backup with a rebuilt file)
New-Item -ItemType Directory -Force $backup | Out-Null
foreach ($f in $files) {
  if ((Test-Path "$dest\$($f.name)") -and -not (Test-Path "$backup\$($f.name)")) {
    Copy-Item "$dest\$($f.name)" $backup
  }
}
# WeaselTSF relaunches WeaselServer on demand, so files may stay locked: rename in-use files aside, then copy
foreach ($f in $files) {
  $target = "$dest\$($f.name)"
  if (Test-Path $target) {
    Remove-Item "$target.old" -Force -ErrorAction SilentlyContinue
    Move-Item $target "$target.old" -Force
  }
  Copy-Item $f.src $target -Force
}
# web settings pages (WeaselDeployer.exe /websettings); back up the old folder once
if ((Test-Path "$dest\web") -and -not (Test-Path "$backup\web")) {
  Copy-Item "$dest\web" "$backup\web" -Recurse
}
Remove-Item "$dest\web" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item "$root\web" "$dest\web" -Recurse -Force
Get-Process WeaselServer, WisdomLLMHost -ErrorAction SilentlyContinue | Stop-Process -Force

# WeaselServer is restarted by the non-elevated caller (must not run as admin)
"done" | Out-File "$root\install_llama_update.done"
Stop-Transcript
