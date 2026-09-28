@echo off
rem Builds and runs the offline tuner for recommend / correction (x64, MSVC), using the installed
rem LLM host (WisdomLLMHost and the llama.cpp DLLs).
rem   run.bat <model.gguf> [--log "%APPDATA%\Rime\personal\choice_log.dat"] [--cases file] [--correct] ...
rem Without --log or --cases it uses cases.txt next to this script.
setlocal
cd /d %~dp0
set "HERE=%~dp0"
set "ROOT=%~dp0..\.."
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined WEASEL_INSTALL set "WEASEL_INSTALL=C:\Program Files\Rime\wisdom-weasel"
set "OUT=%ROOT%\msbuild\TuneChoice"
set "BIN=%OUT%\bin"
if not exist "%BIN%" mkdir "%BIN%"
set "C=%ROOT%\core"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /O2 /DNDEBUG /I"%ROOT%\include" ^
  /Fo"%OUT%\\" /Fe"%BIN%\TuneChoice.exe" ^
  TuneChoice.cpp ^
  "%C%\ime\rescore.cpp" "%C%\ime\calibration.cpp" "%C%\ime\text_rules.cpp" "%C%\ime\rime_helpers.cpp" ^
  "%C%\llm\RemoteLLMProvider.cpp" "%C%\llm\LLMProvider.cpp" "%C%\llm_ipc\client.cpp" ^
  "%C%\crypto\personal_crypto.cpp" "%C%\platform\win\key_store_win.cpp" "%C%\platform\win\process_win.cpp" ^
  "%C%\net\win\http_win.cpp" ^
  /Tc "%C%\third_party\monocypher\monocypher.c" ^
  /link /LIBPATH:"%ROOT%\lib64" rime.lib || exit /b 1
rem The LLM host has to sit next to the executable
for %%f in ("%WEASEL_INSTALL%\WisdomLLMHost.exe" "%WEASEL_INSTALL%\*.dll") do copy /y "%%~f" "%BIN%" >nul
if "%~1"=="" (
  echo usage: run.bat ^<model.gguf^> [--log file] [--cases file] [--correct] [--limit N] [--gpu N] [--base] [--dump file]
  exit /b 2
)
set "MODEL=%~1"
set "REST="
set "HAS_DATA="
:args
shift
if "%~1"=="" goto run
if "%~1"=="--log" set "HAS_DATA=1"
if "%~1"=="--cases" set "HAS_DATA=1"
set REST=%REST% %1
goto args
:run
if not defined HAS_DATA set REST=%REST% --cases "%HERE%cases.txt"
"%BIN%\TuneChoice.exe" "%MODEL%" "%WEASEL_INSTALL%\data" "%OUT%" %REST%
