@echo off
rem Builds and runs the core/ime tests (x64, MSVC). Uses a test key file for the encryption.
setlocal
cd /d %~dp0
set "ROOT=%~dp0..\.."
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%ROOT%\msbuild\TestIme"
if not exist "%OUT%" mkdir "%OUT%"
set "KEY=%OUT%\keys\personal.key"
set "KEY=%KEY:\=\\%"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DNDEBUG /I"%ROOT%\include" /DPERSONAL_CRYPTO_TEST_KEY_FILE=\"%KEY%\" ^
  /Fo"%OUT%\\" /Fe"%OUT%\TestIme.exe" ^
  TestIme.cpp "%ROOT%\core\ime\text_rules.cpp" "%ROOT%\core\ime\rescore.cpp" ^
  "%ROOT%\core\ime\prediction_engine.cpp" ^
  "%ROOT%\core\ime\calibration.cpp" "%ROOT%\core\ime\choice_stats.cpp" "%ROOT%\core\ime\choice_log.cpp" ^
  "%ROOT%\core\personal\PersonalLexicon.cpp" "%ROOT%\core\personal\LearnFilter.cpp" ^
  "%ROOT%\core\crypto\personal_crypto.cpp" "%ROOT%\core\platform\win\key_store_win.cpp" ^
  /Tc "%ROOT%\core\third_party\monocypher\monocypher.c" || exit /b 1
"%OUT%\TestIme.exe"
