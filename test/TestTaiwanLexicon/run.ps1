param(
  [string]$WorkDir = 'D:\wisdom-weasel-audit\taiwan-lexicon-tests',
  [string]$SharedDir = 'C:\Program Files\Rime\wisdom-weasel\data'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
$sandbox = Join-Path $WorkDir ('user-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
$stagedShared = Join-Path $WorkDir ('shared-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stagedShared | Out-Null
Copy-Item -Path (Join-Path $SharedDir '*') -Destination $stagedShared -Recurse -Force
& (Join-Path $repo 'tools\stage_taiwan_lexicon.ps1') -DataDir $stagedShared
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $sandbox 'default.custom.yaml'), "patch:`n  schema_list:`n    - schema: bopomofo`n    - schema: bopomofo_express`n    - schema: bopomofo_tw`n", $utf8)
[IO.File]::WriteAllText((Join-Path $sandbox 'bopomofo_express.custom.yaml'), "patch:`n  translator/dictionary: terra_pinyin.personal`n  translator/user_dict: terra_pinyin`n", $utf8)
[IO.File]::WriteAllText((Join-Path $sandbox 'terra_pinyin.personal.dict.yaml'), "---`nname: terra_pinyin.personal`nversion: '1'`nsort: by_weight`nuse_preset_vocabulary: true`nimport_tables: [terra_pinyin]`n...`n", $utf8)
# Native first deployment must work without invoking the optional Python importer.
$build = @"
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /I"$repo\include" /Fo"$WorkDir\\" /Fe"$WorkDir\TestTaiwanLexicon.exe" "$PSScriptRoot\TestTaiwanLexicon.cpp" /link /LIBPATH:"$repo\lib64" rime.lib
"@
[IO.File]::WriteAllText((Join-Path $WorkDir 'build.cmd'), $build, [Text.Encoding]::ASCII)
& cmd /c (Join-Path $WorkDir 'build.cmd')
if ($LASTEXITCODE) { throw 'Native test compilation failed' }
Copy-Item -LiteralPath (Join-Path $repo 'output\rime.dll') -Destination $WorkDir -Force
& (Join-Path $WorkDir 'TestTaiwanLexicon.exe') $stagedShared $sandbox
if ($LASTEXITCODE) { throw 'Native dictionary lookup test failed' }
