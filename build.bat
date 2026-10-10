@echo off
rem usage: build [Release|Debug|RelWithDebInfo] [target]
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=RelWithDebInfo

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo Visual Studio not found & exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1

set "BDIR=%~dp0build\x64-%CFG%"
cmake -S "%~dp0." -B "%BDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
if "%2"=="" (cmake --build "%BDIR%" -- -k 0) else (cmake --build "%BDIR%" --target %2 -- -k 0)
