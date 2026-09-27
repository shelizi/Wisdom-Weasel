# Downloads the WebView2 SDK (headers + static loader) used by the web settings window into deps\webview2.
param([string]$Version = '1.0.4191.47')
$ErrorActionPreference = 'Stop'
$dest = Join-Path $PSScriptRoot 'deps\webview2'
if (Test-Path (Join-Path $dest 'include\WebView2.h')) {
  Write-Host "WebView2 SDK already in $dest"
  exit 0
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "webview2-$Version"
$zip = "$tmp.zip"
Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg" -OutFile $zip
Expand-Archive $zip $tmp -Force
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $tmp 'build\native\include') $dest -Recurse -Force
foreach ($arch in 'x86', 'x64', 'arm64') {
  New-Item -ItemType Directory -Force (Join-Path $dest $arch) | Out-Null
  Copy-Item (Join-Path $tmp "build\native\$arch\WebView2LoaderStatic.lib") (Join-Path $dest $arch)
}
Copy-Item (Join-Path $tmp 'LICENSE.txt') $dest -ErrorAction SilentlyContinue
Remove-Item $tmp, $zip -Recurse -Force
Write-Host "WebView2 SDK $Version installed to $dest"
