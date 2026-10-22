@echo off
rem Windows equivalent of `make release` / `make debug`. Usage: scripts\build-windows.cmd [release^|debug]
setlocal
set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=release
set CMAKE_TYPE=Debug
if /I "%BUILD_TYPE%"=="release" set CMAKE_TYPE=Release

if defined VSCMD_VER goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
rem Forward slashes: the root directory ends up in C string literals
for %%i in ("%~dp0..") do set "ROOT=%%~fi"
set "ROOT=%ROOT:\=/%"
cmake -G Ninja -DCMAKE_BUILD_TYPE=%CMAKE_TYPE% -DEXTENSION_STATIC_BUILD=1 ^
	-DDUCKDB_EXTENSION_CONFIGS="%ROOT%/extension_config.cmake" ^
	-DUNITTEST_ROOT_DIRECTORY="%ROOT%/" -DBENCHMARK_ROOT_DIRECTORY="%ROOT%/" ^
	-DENABLE_UNITTEST_CPP_TESTS=FALSE ^
	-S "%ROOT%/duckdb" -B "%ROOT%/build/%BUILD_TYPE%" || exit /b 1
cmake --build "%ROOT%/build/%BUILD_TYPE%" --config %CMAKE_TYPE% || exit /b 1
