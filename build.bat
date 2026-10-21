@echo off
rem Build script for OpenAES67 (standalone, no CMake needed).
rem
rem Usage:
rem   build.bat          default build (WASAPI only)
rem   build.bat asio     also build the ASIO backend
rem
rem The source list is globbed, not hand-written. It used to be a literal list and
rem that list caused two bugs: receive_loop.cpp was missing from the core set
rem (unresolved runReceiveLoop) while being wrongly appended to the self-test line
rem (duplicate symbols). A new src\*.cpp file is now picked up automatically.
rem
rem src\gui\main_gui.cpp is deliberately not globbed: it needs Qt and is built by
rem CMake (see CMakeLists.txt).
setlocal enabledelayedexpansion
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1
cd /d "%~dp0"
if not exist build mkdir build

rem --- optional ASIO backend ---------------------------------------------------
rem The Steinberg ASIO SDK is not redistributable, so it is not bundled. Put
rem iasiodrv.h / asio.h / asiosys.h into third_party\asio and build with "build.bat asio".
set ASIO_DEF=
set ASIO_INC=
rem Note: "ASIO=1" cannot be used as a flag -- cmd treats "=" as an argument
rem separator, so %~1 would only ever be "ASIO". Use a bare word.
if /i "%~1"=="asio" (
    if not exist "third_party\asio\iasiodrv.h" (
        echo === build FAILED: asio requested but third_party\asio\iasiodrv.h not found ===
        exit /b 1
    )
    set ASIO_DEF=/DOPENAES67_WITH_ASIO
    set ASIO_INC=/I"third_party\asio"
    echo === ASIO backend enabled ===
)

rem --- core sources (every src\*.cpp except the CLI entry point) ---------------
set CORE=
for %%f in (src\*.cpp) do (
    if /i not "%%~nxf"=="main.cpp" set CORE=!CORE! %%f
)
if "!CORE!"=="" (
    echo === build FAILED: no sources found under src ===
    exit /b 1
)

rem Resources: icon + version info. rc.exe comes from the Windows SDK that
rem VsDevCmd.bat puts on PATH.
echo === compiling resources ===
rc /nologo /fo build\app.res assets\app.rc
if errorlevel 1 goto :fail

echo === building selftest ===
cl /nologo /EHsc /std:c++17 /W4 /O2 /Oi /Gy /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX !ASIO_DEF! /I include !ASIO_INC! ^
   tests\selftest.cpp !CORE! ^
   build\app.res ws2_32.lib ole32.lib uuid.lib advapi32.lib iphlpapi.lib winmm.lib shell32.lib /Fe:build\selftest.exe /Fo:build\
if errorlevel 1 goto :fail

echo === building openaes67 CLI ===
cl /nologo /EHsc /std:c++17 /W4 /O2 /Oi /Gy /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX !ASIO_DEF! /I include !ASIO_INC! ^
   src\main.cpp !CORE! ^
   build\app.res ws2_32.lib ole32.lib uuid.lib advapi32.lib iphlpapi.lib winmm.lib shell32.lib /Fe:build\openaes67.exe /Fo:build\
if errorlevel 1 goto :fail


rem Gate: the numbers and flags stated in the documentation must still hold.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\verify-claims.ps1"
if errorlevel 1 (
    echo === CLAIMS BROKEN - fix docs or code before committing ===
    exit /b 1
)

rem Performance gate: opt-in, because it needs the machine to be otherwise idle and
rem a timing gate that runs on every build poisons the build for everyone. Set
rem OPENAES67_PERF_GATE=1 before building to compare against bench-baseline.txt.
if /i "%OPENAES67_PERF_GATE%"=="1" (
    echo === checking benchmark baseline ===
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\verify-perf.ps1
    if errorlevel 1 goto :fail
)

echo === build OK ===
exit /b 0

:fail
echo === build FAILED ===
exit /b 1
