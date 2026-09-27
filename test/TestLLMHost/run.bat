@echo off
rem Builds and runs the LLM host IPC tests (x64, MSVC).
setlocal
cd /d %~dp0
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%~dp0..\..\msbuild\TestLLMHost"
if not exist "%OUT%" mkdir "%OUT%"
set "CORE=..\..\core"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /Fo"%OUT%\\" /Fe"%OUT%\FakeHost.exe" ^
  FakeHost.cpp %CORE%\llm_ipc\host.cpp %CORE%\platform\win\process_win.cpp || exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /Fo"%OUT%\\" /Fe"%OUT%\TestLLMHost.exe" ^
  TestLLMHost.cpp %CORE%\llm_ipc\client.cpp %CORE%\platform\win\process_win.cpp || exit /b 1
"%OUT%\TestLLMHost.exe"
