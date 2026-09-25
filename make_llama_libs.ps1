# Generate x64 import libs for llama.cpp DLLs and stage headers where WeaselServer.vcxproj expects them
param([string]$LlamaDir = "C:\Users\zex55\src\llama-b11177")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" | Sort-Object Name -Descending | Select-Object -First 1
$bin = "$($msvc.FullName)\bin\Hostx64\x64"

$tmp = "$root\deps\llama-implib"
New-Item -ItemType Directory -Force $tmp | Out-Null
foreach ($name in 'llama', 'ggml', 'ggml-base') {
  $dll = "$LlamaDir\bin\$name.dll"
  $exports = & "$bin\dumpbin.exe" /exports $dll |
    Where-Object { $_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' } |
    ForEach-Object { $Matches[1] }
  $def = "$tmp\$name.def"
  @("LIBRARY `"$name.dll`"", 'EXPORTS') + $exports | Set-Content $def -Encoding ascii
  & "$bin\lib.exe" /nologo /machine:x64 "/def:$def" "/out:$root\lib64\$name.lib" | Out-Null
  Write-Host "$name.lib: $($exports.Count) exports"
}

# headers: $(SolutionDir)\llamatest\llama.cpp\include and \llamatest\ggml\include
$src = Get-ChildItem $LlamaDir -Directory -Filter 'llama.cpp-*' | Select-Object -First 1
New-Item -ItemType Directory -Force "$root\llamatest\llama.cpp", "$root\llamatest\ggml" | Out-Null
Copy-Item "$($src.FullName)\include" "$root\llamatest\llama.cpp" -Recurse -Force
Copy-Item "$($src.FullName)\ggml\include" "$root\llamatest\ggml" -Recurse -Force
Write-Host "headers staged from $($src.Name)"
