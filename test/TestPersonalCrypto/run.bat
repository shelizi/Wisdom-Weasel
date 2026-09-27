@echo off
rem Builds and runs the personal data encryption tests (x64, MSVC).
setlocal
cd /d %~dp0
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%~dp0..\..\msbuild\TestPersonalCrypto"
if not exist "%OUT%" mkdir "%OUT%"
set "KEY=%OUT%\keys\personal.key"
set "KEY=%KEY:\=\\%"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /I..\..\include /DPERSONAL_CRYPTO_TEST_KEY_FILE=\"%KEY%\" ^
  /Fo"%OUT%\\" /Fe"%OUT%\TestPersonalCrypto.exe" ^
  TestPersonalCrypto.cpp ..\..\core\crypto\personal_crypto.cpp ..\..\core\platform\win\key_store_win.cpp ^
  /Tc ..\..\core\third_party\monocypher\monocypher.c || exit /b 1
"%OUT%\TestPersonalCrypto.exe"
