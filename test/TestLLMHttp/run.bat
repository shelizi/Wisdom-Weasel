@echo off
rem Builds and runs the LLM HTTP tests against mock_server.py (x64, MSVC, Python 3).
rem The LLM code in core/ builds without ATL or the Weasel utilities.
rem Then runs the built WisdomLLMHost.exe end to end (DLLs come from the installed IME folder).
setlocal
cd /d %~dp0
set "ROOT=%~dp0..\.."
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VCINSTALLDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined WEASEL_INSTALL set "WEASEL_INSTALL=C:\Program Files\Rime\wisdom-weasel"
set "OUT=%ROOT%\msbuild\TestLLMHttp"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\host" mkdir "%OUT%\host"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DNDEBUG /I"%ROOT%\include" ^
  /Fo"%OUT%\\" /Fe"%OUT%\TestLLMHttp.exe" ^
  TestLLMHttp.cpp "%ROOT%\core\llm\LLMProvider.cpp" "%ROOT%\core\net\win\http_win.cpp" ^
  /link /LIBPATH:"%ROOT%\lib64" rime.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /MT /DNDEBUG /I"%ROOT%\include" ^
  /Fo"%OUT%\host\\" /Fe"%OUT%\host\HostSmoke.exe" ^
  HostSmoke.cpp "%ROOT%\core\llm\RemoteLLMProvider.cpp" "%ROOT%\core\llm\LLMProvider.cpp" ^
  "%ROOT%\core\llm_ipc\client.cpp" "%ROOT%\core\platform\win\process_win.cpp" ^
  "%ROOT%\core\net\win\http_win.cpp" ^
  /link /LIBPATH:"%ROOT%\lib64" rime.lib || exit /b 1
copy /y "%ROOT%\output\rime.dll" "%OUT%" >nul
copy /y "%ROOT%\output\WisdomLLMHost.exe" "%OUT%\host" >nul
copy /y "%ROOT%\output\rime.dll" "%OUT%\host" >nul
set PORT=18765
start "" /b python mock_server.py %PORT%
rem wait for the server to listen
powershell -NoProfile -Command "for($i=0;$i -lt 50;$i++){try{(New-Object Net.Sockets.TcpClient('127.0.0.1',%PORT%)).Close();exit 0}catch{Start-Sleep -Milliseconds 100}};exit 1" || exit /b 1
"%OUT%\TestLLMHttp.exe" %PORT%
set RESULT=%errorlevel%
rem llama.cpp DLLs: the installed IME, or output\llama (get-llama-runtime.ps1, used in CI)
set "PATH=%WEASEL_INSTALL%;%ROOT%\output\llama;%PATH%"
"%OUT%\host\HostSmoke.exe" %PORT%
if errorlevel 1 set RESULT=1
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name like 'python%%' and CommandLine like '%%mock_server.py %PORT%%%'\" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
exit /b %RESULT%
