@echo off
rem Builds and runs the learned ranker smoke test (x64, MSVC).
setlocal
cd /d %~dp0
set "ROOT=%~dp0..\.."
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%ROOT%\msbuild\TestRanker"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DNDEBUG /I"%ROOT%\include" ^
  /Fo"%OUT%\\" /Fe"%OUT%\TestRanker.exe" ^
  TestRanker.cpp "%ROOT%\core\ime\ranker.cpp" || exit /b 1
"%OUT%\TestRanker.exe"
