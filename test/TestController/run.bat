@echo off
rem Builds and runs the ime::Controller tests against librime with the installed schema data (x64, MSVC).
rem Builds everything in core/ that the controller uses, without the Weasel headers.
setlocal
cd /d %~dp0
set "ROOT=%~dp0..\.."
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined RIME_SHARED set "RIME_SHARED=C:\Program Files\Rime\wisdom-weasel\data"
set "OUT=%ROOT%\msbuild\TestController"
if not exist "%OUT%" mkdir "%OUT%"
set "KEY=%OUT%\keys\personal.key"
set "KEY=%KEY:\=\\%"
set "C=%ROOT%\core"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DNDEBUG /I"%ROOT%\include" /DPERSONAL_CRYPTO_TEST_KEY_FILE=\"%KEY%\" ^
  /Fo"%OUT%\\" /Fe"%OUT%\TestController.exe" ^
  TestController.cpp ^
  "%C%\ime\controller.cpp" "%C%\ime\prediction_engine.cpp" "%C%\ime\rescore.cpp" ^
  "%C%\ime\text_rules.cpp" "%C%\ime\choice_stats.cpp" "%C%\ime\choice_log.cpp" "%C%\ime\rime_helpers.cpp" ^
  "%C%\llm\ContextHistory.cpp" "%C%\llm\MemoryCompressor.cpp" "%C%\llm\LLMProvider.cpp" ^
  "%C%\llm\RemoteLLMProvider.cpp" "%C%\llm_ipc\client.cpp" ^
  "%C%\personal\PersonalLexicon.cpp" "%C%\personal\PersonalRefiner.cpp" ^
  "%C%\crypto\personal_crypto.cpp" "%C%\platform\win\key_store_win.cpp" "%C%\platform\win\process_win.cpp" ^
  "%C%\net\win\http_win.cpp" ^
  /Tc "%C%\third_party\monocypher\monocypher.c" ^
  /link /LIBPATH:"%ROOT%\lib64" rime.lib || exit /b 1
copy /y "%ROOT%\output\rime.dll" "%OUT%" >nul
"%OUT%\TestController.exe" "%RIME_SHARED%" "%OUT%\user"
