@echo off
rem SPDX-License-Identifier: MIT OR Apache-2.0
rem
rem Configure, build and test on Windows with whatever Visual Studio is
rem installed. Nothing needs to be on PATH and no developer prompt is needed:
rem vswhere locates the installation, and the Visual Studio generator finds the
rem C++ toolset itself.
rem
rem     scripts\build-msvc.bat            configure + build + ctest
rem     scripts\build-msvc.bat install    the same, then cmake --install
rem     scripts\build-msvc.bat bench      the same, then one benchmark run
rem
rem Tools are invoked by full path where they were found, which matters: cmd
rem behaves badly when a command is resolved through a PATH entry containing a
rem percent sign, and such entries are common on real developer machines.

setlocal enabledelayedexpansion
cd /d "%~dp0.." || exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :found_vswhere
echo vswhere was not found. Install the Visual Studio Installer, or run these
echo commands yourself from a Developer Prompt:
echo     cmake -S . -B build
echo     cmake --build build --config Release
echo     ctest --test-dir build -C Release --output-on-failure
exit /b 1

:found_vswhere
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if defined VSROOT goto :have_vsroot
echo No Visual Studio installation with the C++ toolset was found.
exit /b 1

:have_vsroot
echo Using "!VSROOT!"
set "VS_CMAKE_BIN=!VSROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if exist "!VS_CMAKE_BIN!\cmake.exe" goto :have_cmake
set "CMAKE=cmake"
set "CTEST=ctest"
goto :configure

:have_cmake
set "CMAKE=!VS_CMAKE_BIN!\cmake.exe"
set "CTEST=!VS_CMAKE_BIN!\ctest.exe"

:configure
"%CMAKE%" -S . -B build || exit /b 1
"%CMAKE%" --build build --config Release || exit /b 1
"%CTEST%" --test-dir build -C Release --output-on-failure || exit /b 1

if /i not "%~1"=="install" goto :maybe_bench
"%CMAKE%" --install build --config Release --prefix "%TEMP%\bitlane-prefix" || exit /b 1
echo Installed to "%TEMP%\bitlane-prefix".
echo Point a consumer project at it with -DCMAKE_PREFIX_PATH and then use
echo find_package with a CONFIG call for bitlane.

:maybe_bench
if /i not "%~1"=="bench" goto :finish
"%CD%\build\Release\bitlane_bench.exe"

:finish
echo Done. Also worth running: build\Release\bitlane_bench.exe,
echo   build\Release\example_posting_index.exe and build\Release\example_tag_query.exe
