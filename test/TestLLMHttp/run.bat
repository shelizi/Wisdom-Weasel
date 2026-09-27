@echo off
rem Builds and runs the LLM HTTP tests against mock_server.py (x64, MSVC, Python 3).
rem The LLM code in core/ builds without ATL or the Weasel utilities.
setlocal
cd /d %~dp0
set "ROOT=%~dp0..\.."
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%ROOT%\msbuild\TestLLMHttp"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DNDEBUG /I"%ROOT%\include" ^
  /Fo"%OUT%\\" /Fe"%OUT%\TestLLMHttp.exe" ^
  TestLLMHttp.cpp "%ROOT%\core\llm\LLMProvider.cpp" "%ROOT%\core\net\win\http_win.cpp" ^
  /link /LIBPATH:"%ROOT%\lib64" rime.lib || exit /b 1
copy /y "%ROOT%\output\rime.dll" "%OUT%" >nul
set PORT=18765
start "" /b python mock_server.py %PORT%
rem wait for the server to listen
powershell -NoProfile -Command "for($i=0;$i -lt 50;$i++){try{(New-Object Net.Sockets.TcpClient('127.0.0.1',%PORT%)).Close();exit 0}catch{Start-Sleep -Milliseconds 100}};exit 1" || exit /b 1
"%OUT%\TestLLMHttp.exe" %PORT%
set RESULT=%errorlevel%
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name like 'python%%' and CommandLine like '%%mock_server.py %PORT%%%'\" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
exit /b %RESULT%
