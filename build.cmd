@echo off
rem Configure + build with MSVC x64 and Ninja. Output: build\*.exe (or the directory given as %1, relative to the repo).
setlocal
set "BUILD_DIR=%~dp0build"
if not "%~1"=="" set "BUILD_DIR=%~f1"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VULKAN_SDK for /d %%i in (C:\VulkanSDK\*) do set "VULKAN_SDK=%%i"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem VS includes Ninja in its CMake tools, so no separate Ninja download is needed.
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
if "%~2"=="install" goto install_targets
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
exit /b 0
:install_targets
cmake --build "%BUILD_DIR%" --target gt2game gt2install gt2media gt2bootcapture gt2hdchecks gt2checks gt2wheelchecks gt2vrchecks gt2drivingchecks gt2questchecks
if errorlevel 1 exit /b 1
exit /b 0
