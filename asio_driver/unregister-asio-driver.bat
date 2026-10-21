@echo off
rem Removes the OpenAES67 ASIO driver registration. Needs an elevated prompt.
setlocal EnableDelayedExpansion
set GUID={7C1F2E64-9A3B-4C5D-8E7F-0A1B2C3D4E5F}
set NAME=OpenAES67 ASIO 64x64

echo === unregistering %NAME% ===

net session >nul 2>&1
if errorlevel 1 goto :no_admin

reg delete "HKLM\SOFTWARE\ASIO\%NAME%" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\WOW6432Node\ASIO\%NAME%" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Classes\CLSID\%GUID%" /f >nul 2>&1

echo.
echo === UNREGISTERED ===
echo   The DLL itself was left in place; delete asio_driver\out\openaes67_asio64.dll
echo   if you want it gone.
exit /b 0

:no_admin
echo.
echo === NOT ELEVATED ===
echo   Right-click the prompt and choose "Run as administrator".
exit /b 2
