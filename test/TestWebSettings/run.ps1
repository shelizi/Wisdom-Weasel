# 網頁版設定的整合測試：在沙盒使用者資料夾裡真的套用並重新部署，再重新開啟確認讀回來的值。
# 真正的使用者資料夾不會被寫入（只複製頂層的設定檔）。重新部署時正在執行的輸入法會暫停片刻。
param(
  [string]$Install = 'C:\Program Files\Rime\wisdom-weasel',  # 共用資料（方案檔）從這裡的 data 取
  [string]$UserDir = "$env:APPDATA\Rime"                       # 複製設定檔的來源
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$work = Join-Path $root 'msbuild\TestWebSettings'
$bin = Join-Path $work 'bin'
$sandbox = Join-Path $work 'user'

# 沙盒：設定程式與共用資料
if (Test-Path $work) { cmd /c rmdir /s /q "$work" | Out-Null }
New-Item -ItemType Directory -Force $bin, $sandbox | Out-Null
foreach ($f in 'WeaselDeployer.exe', 'rime.dll', 'WinSparkle.dll') { Copy-Item (Join-Path $root "output\$f") $bin }
cmd /c mklink /J "$bin\data" "$Install\data" | Out-Null
# 使用者設定：頂層的 yaml；語言模型檔用硬連結（不佔空間，下載測試只會取代沙盒裡的連結）
Get-ChildItem $UserDir -File -Filter *.yaml | Copy-Item -Destination $sandbox
# 設定程式讀的是部署後的 build\*.yaml：一起複製，才會從目前的設定出發
New-Item -ItemType Directory -Force "$sandbox\build" | Out-Null
Get-ChildItem "$UserDir\build" -File -Filter *.yaml | Copy-Item -Destination "$sandbox\build"
Get-ChildItem $UserDir -File -Filter *.gram | ForEach-Object { cmd /c mklink /H "$sandbox\$($_.Name)" $_.FullName | Out-Null }

$env:WEASEL_TEST_USER_DIR = $sandbox
$env:WISDOM_SETTINGS_WEB = Join-Path $root 'web\settings'
foreach ($phase in 'apply', 'verify') {
  $out = Join-Path $work "$phase.json"
  Remove-Item $out -ErrorAction SilentlyContinue  # 沙盒沒能整個清掉時，也不會讀到上次的結果
  $p = Start-Process "$bin\WeaselDeployer.exe" -ArgumentList "/websettings --selftest `"$out`" $phase" -PassThru
  if (-not $p.WaitForExit(6 * 60 * 1000)) { $p.Kill(); throw "$phase timed out" }
  if (-not (Test-Path $out)) { throw "$phase produced no result (exit $($p.ExitCode))" }
  "$phase done"
}
python (Join-Path $PSScriptRoot 'verify.py') $work
exit $LASTEXITCODE
