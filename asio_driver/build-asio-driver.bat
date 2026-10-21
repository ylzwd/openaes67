@echo off
rem Builds the OpenAES67 ASIO driver (64 in / 64 out) and its host test.
rem
rem The ASIO constants live in asio_driver\asio_abi.h, written for this project
rem because Steinberg's SDK is not redistributable. If you do have the SDK, put
rem iasiodrv.h in third_party\asio and add /DOPENAES67_USE_OFFICIAL_ASIO_SDK.
rem
rem Paths are relative on purpose: the repository lives under a directory whose
rem name is not ASCII, and passing that absolute path on the cl command line made
rem cl report "missing source file name" (D8003).
setlocal EnableDelayedExpansion
pushd "%~dp0.."

echo === OpenAES67 ASIO driver ===

rem --- toolchain -------------------------------------------------------------
set VS=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VS=%%i
if not defined VS goto :no_toolchain
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" goto :no_toolchain
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto :no_toolchain

if not exist asio_driver\out mkdir asio_driver\out

set COMMON=/nologo /W4 /utf-8 /O2 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /Iinclude /Iasio_driver

echo   compiling shared ring
cl %COMMON% /c src\shared_audio_ring.cpp /Foasio_driver\out\shared_audio_ring.obj
if errorlevel 1 goto :failed

echo   compiling driver dll
cl %COMMON% /LD /DOPENAES67_ASIO_EXPORTS ^
   asio_driver\openaes67_asio64.cpp asio_driver\out\shared_audio_ring.obj ^
   /Feasio_driver\out\openaes67_asio64.dll /Foasio_driver\out\\ ^
   /link /DEF:asio_driver\openaes67_asio64.def /SUBSYSTEM:WINDOWS ^
   ole32.lib oleaut32.lib advapi32.lib winmm.lib
if errorlevel 1 goto :failed

echo   compiling host test
cl %COMMON% asio_driver\asio_host_test.cpp asio_driver\out\shared_audio_ring.obj ^
   /Feasio_driver\out\asio_host_test.exe /Foasio_driver\out\\ ^
   /link /SUBSYSTEM:CONSOLE ole32.lib oleaut32.lib
if errorlevel 1 goto :failed

echo.
echo === BUILD OK ===
echo   asio_driver\out\openaes67_asio64.dll
echo   asio_driver\out\asio_host_test.exe
echo.
echo Next:
echo   asio_driver\out\asio_host_test.exe 2        (no admin needed)
echo   asio_driver\register-asio-driver.bat        (elevated, for DAWs)
popd
exit /b 0

:no_toolchain
echo.
echo === TOOLCHAIN MISSING ===
echo   Visual Studio 2022 with the C++ x64 toolset was not found.
popd
exit /b 1

:failed
echo.
echo === BUILD FAILED ===
popd
exit /b 1
