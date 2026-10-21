@echo off
rem Registers the OpenAES67 ASIO driver so ASIO hosts (Audition, Reaper, ...) list it.
rem Needs an elevated prompt: it writes under HKLM.
setlocal EnableDelayedExpansion
set ROOT=%~dp0
set DLL=%ROOT%out\openaes67_asio64.dll
set GUID={7C1F2E64-9A3B-4C5D-8E7F-0A1B2C3D4E5F}
set NAME=OpenAES67 ASIO 64x64

echo === registering %NAME% ===

if not exist "%DLL%" goto :no_dll

net session >nul 2>&1
if errorlevel 1 goto :no_admin

rem COM in-process server: the class id must point at the DLL.
reg add "HKLM\SOFTWARE\Classes\CLSID\%GUID%\InprocServer32" /ve /t REG_SZ /d "%DLL%" /f >nul
reg add "HKLM\SOFTWARE\Classes\CLSID\%GUID%\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f >nul
reg add "HKLM\SOFTWARE\Classes\CLSID\%GUID%" /v AppID /t REG_SZ /d "" /f >nul 2>&1

rem The key ASIO hosts enumerate. Both registry views: a 32-bit host reads the
rem WOW6432Node copy, a 64-bit host reads the native one.
reg add "HKLM\SOFTWARE\ASIO\%NAME%" /v CLSID /t REG_SZ /d "%GUID%" /f >nul
reg add "HKLM\SOFTWARE\ASIO\%NAME%" /v Description /t REG_SZ /d "OpenAES67 64 in / 64 out virtual sound card" /f >nul
reg add "HKLM\SOFTWARE\WOW6432Node\ASIO\%NAME%" /v CLSID /t REG_SZ /d "%GUID%" /f >nul
reg add "HKLM\SOFTWARE\WOW6432Node\ASIO\%NAME%" /v Description /t REG_SZ /d "OpenAES67 64 in / 64 out virtual sound card" /f >nul
if errorlevel 1 goto :failed

echo.
echo === REGISTERED ===
echo   CLSID : %GUID%
echo   DLL   : %DLL%
echo   key   : HKLM\SOFTWARE\ASIO\%NAME%
echo.
echo Restart the ASIO host application (Audition) and it should appear as
echo   "%NAME%".
echo Unregister with: asio_driver\unregister-asio-driver.bat
exit /b 0

:no_dll
echo.
echo === DLL MISSING ===
echo   %DLL% not found. Build it first: asio_driver\build-asio-driver.bat
exit /b 1

:no_admin
echo.
echo === NOT ELEVATED ===
echo   Right-click the prompt and choose "Run as administrator": the ASIO key
echo   lives under HKLM.
exit /b 2

:failed
echo.
echo === REGISTRATION FAILED ===
exit /b 1
