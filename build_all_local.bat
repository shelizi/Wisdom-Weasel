@echo off
rem Local Release build of server (x64) + in-process TSF/IME DLLs (x64 and Win32), VS 2026 / Boost 1.83.
rem Never runs "weaselserver /q" (unlike build.bat).
setlocal
set NoDefaultCurrentDirectoryInExePath=
cd /d %~dp0
call "%~dp0env.bat"

set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set B2_COMMON=--with-serialization define=BOOST_USE_WINAPI_VERSION=0x0603 toolset=%BJAM_TOOLSET% link=static runtime-link=static --build-type=complete architecture=x86 stage
pushd "%BOOST_ROOT%"
if not exist b2.exe call "%BOOST_ROOT%\bootstrap.bat" vc143 || exit /b 1
if not exist "%BOOST_ROOT%\stage\lib\libboost_wserialization-vc143-mt-s-x64-1_83.lib" (
  "%BOOST_ROOT%\b2.exe" -j%NUMBER_OF_PROCESSORS% --user-config="%BOOST_ROOT%\user-config.jam" address-model=64 %B2_COMMON% || exit /b 1
)
if not exist "%BOOST_ROOT%\stage\lib\libboost_wserialization-vc143-mt-s-x32-1_83.lib" (
  "%BOOST_ROOT%\b2.exe" -j%NUMBER_OF_PROCESSORS% --user-config="%BOOST_ROOT%\user-config-x86.jam" address-model=32 %B2_COMMON% || exit /b 1
)
popd

set VERSION_MAJOR=0
set VERSION_MINOR=17
set VERSION_PATCH=4
set PRODUCT_VERSION=0.17.4.0
set FILE_VERSION=0.17.4.0
powershell -NoProfile -Command "$t=[IO.File]::ReadAllText('%~dp0weasel.props.template');foreach($v in 'BOOST_ROOT','PLATFORM_TOOLSET','VERSION_MAJOR','VERSION_MINOR','VERSION_PATCH','PRODUCT_VERSION','FILE_VERSION'){$t=$t.Replace('$'+$v,[Environment]::GetEnvironmentVariable($v))};[IO.File]::WriteAllText('%~dp0weasel.props',$t)" || exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_buildinfo.ps1"
msbuild.exe "%~dp0weasel.sln" /t:WeaselServer;WeaselDeployer;WeaselTSF;WeaselIME /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=%PLATFORM_TOOLSET% /v:minimal || exit /b 1
msbuild.exe "%~dp0weasel.sln" /t:WeaselTSF;WeaselIME /m /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=%PLATFORM_TOOLSET% /v:minimal || exit /b 1
exit /b 0
