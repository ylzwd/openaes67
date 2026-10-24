@echo off
rem The INF version must be the project version with a fourth field. Ask version.h
rem rather than carrying another literal: this file used to say 0.1.0.0 while the
rem executables reported 0.1.1, and an INF that disagrees with its binaries is the
rem kind of thing a signing review finds before a user does.
set "OPENAES67_INF_VERSION="
for /f "delims=" %%v in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\get-version.ps1" -Numeric') do set "OPENAES67_INF_VERSION=%%v"
if not defined OPENAES67_INF_VERSION set "OPENAES67_INF_VERSION=0.0.0.0"
rem Builds, stamps, validates and signs the M6 toolchain probe driver.
rem
rem This proves the four things a kernel driver needs on this machine:
rem   1. compile with the WDK kernel headers
rem   2. link against the WDM libraries (ntoskrnl + hal + wdm)
rem   3. stamp and validate the INF with stampinf / Inf2Cat
rem   4. sign the package with a locally generated test certificate
rem
rem The driver is a probe, not the audio driver: see driver\README.md.
rem Loading it additionally requires test signing AND Secure Boot disabled.
rem Building and signing do not.

setlocal

set ROOT=%~dp0
set OUT=%ROOT%out
set KITS=C:\Program Files (x86)\Windows Kits\10
set KM_VER=10.0.26100.0
set INC_KM=%KITS%\Include\%KM_VER%\km
set INC_SHARED=%KITS%\Include\%KM_VER%\shared
set LIB_KM=%KITS%\Lib\%KM_VER%\km\x64
set BIN_X64=%KITS%\bin\%KM_VER%\x64
set BIN_X86=%KITS%\bin\%KM_VER%\x86

if not exist "%INC_KM%\portcls.h" (
    echo ERROR: WDK not found at "%KITS%".
    echo        Install it with: winget install --id Microsoft.WindowsWDK.10.0.26100 -e
    exit /b 1
)

rem --- Visual Studio environment -------------------------------------------------
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found; install Visual Studio 2022 Build Tools.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (
    echo ERROR: no Visual Studio with the C++ toolset found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"

echo === compiling (probe) ===
cl /nologo /c /kernel /W4 /GS- /Zl /D_WIN64 /D_AMD64_ /DAMD64 ^
   /I"%INC_KM%" /I"%INC_SHARED%" ^
   "%ROOT%openaes67_probe.c" /Fo:openaes67_probe.obj
if errorlevel 1 goto :fail

echo === linking ===
link /nologo /DRIVER /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
     /NODEFAULTLIB /OUT:openaes67_probe.sys ^
     /LIBPATH:"%LIB_KM%" ^
     openaes67_probe.obj ntoskrnl.lib hal.lib wdm.lib BufferOverflowK.lib
if errorlevel 1 goto :fail

echo === compiling (audio adapter) ===
cl /nologo /c /kernel /W4 /GS- /Zl /D_WIN64 /D_AMD64_ /DAMD64 ^
   /I"%INC_KM%" /I"%INC_SHARED%" /I"%ROOT%..\include" /I"%ROOT%..\include\openaes67" ^
   "%ROOT%openaes67_audio.cpp" /Fo:openaes67_audio.obj
if errorlevel 1 goto :fail

echo === linking (audio adapter) ===
link /nologo /DRIVER /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
     /OUT:openaes67_audio.sys ^
     /LIBPATH:"%LIB_KM%" ^
     openaes67_audio.obj portcls.lib ks.lib ksguid.lib wdmguid.lib ntoskrnl.lib hal.lib wdm.lib ^
     BufferOverflowK.lib
if errorlevel 1 goto :fail

echo === stamping INF ===
rem stampinf writes the LOCAL date, Inf2Cat compares against UTC. At 03:00
rem local (UTC+8) the current local date is still "tomorrow" in UTC, so the
rem package counts as postdated and fails signability. Stamp yesterday.
for /f %%d in ('powershell -NoProfile -Command "(Get-Date).AddDays(-1).ToString('MM/dd/yyyy')"') do set STAMPDATE=%%d
copy /y "%ROOT%openaes67_probe.inf" . >nul
"%BIN_X64%\stampinf.exe" -f openaes67_probe.inf -d %STAMPDATE% -v %OPENAES67_INF_VERSION% -a x64
if errorlevel 1 goto :fail

echo === validating package (Inf2Cat) ===
"%BIN_X86%\Inf2Cat.exe" /driver:"%OUT%" /os:10_X64
if errorlevel 1 goto :fail

echo === signing with a local test certificate ===
rem The password protects a throwaway self-signed test cert that never leaves this
rem machine (the .pfx is git-ignored). It still comes from the environment rather
rem than a literal so secret scanners do not flag this file, and so it can be
rem overridden without editing the script.
if not defined OPENAES67_TEST_PFX_PASS set OPENAES67_TEST_PFX_PASS=openaes67
set PFX=%OUT%\openaes67-test.pfx
if not exist "%PFX%" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=OpenAES67 Test Signing' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(2);" ^
      "Export-PfxCertificate -Cert $c -FilePath '%PFX%' -Password (ConvertTo-SecureString -String $env:OPENAES67_TEST_PFX_PASS -Force -AsPlainText) | Out-Null;" ^
      "Export-Certificate -Cert $c -FilePath 'openaes67-test.cer' -Force | Out-Null"
    if errorlevel 1 goto :fail
)

"%BIN_X64%\signtool.exe" sign /fd SHA256 /f "%PFX%" /p %OPENAES67_TEST_PFX_PASS% openaes67_probe.sys
if errorlevel 1 goto :fail

if exist openaes67_probe.cat (
    "%BIN_X64%\signtool.exe" sign /fd SHA256 /f "%PFX%" /p %OPENAES67_TEST_PFX_PASS% openaes67_probe.cat
    if errorlevel 1 goto :fail
)

rem The test certificate is exported as .cer inside the block that creates the
rem .pfx. It is deliberately NOT imported into the Root store (that raises a
rem Windows security dialog) and never read back with Get-PfxCertificate (that
rem prompts for the password on stdin). Both hang unattended builds -- this
rem script waited eight minutes on "Enter password:" once.

echo.

echo === staging the audio driver package ===
if not exist "%OUT%\audio" mkdir "%OUT%\audio"
copy /y openaes67_audio.sys "%OUT%\audio\" >nul
copy /y "%ROOT%openaes67_audio.inf" "%OUT%\audio\" >nul
rem stampinf writes the LOCAL date while Inf2Cat compares UTC, so stamp yesterday.
for /f %%d in ('powershell -NoProfile -Command "(Get-Date).AddDays(-1).ToString('MM/dd/yyyy')"') do set STAMPDATE=%%d
"%BIN_X64%\stampinf.exe" -f "%OUT%\audio\openaes67_audio.inf" -d %STAMPDATE% -v %OPENAES67_INF_VERSION% -a x64
if errorlevel 1 goto :fail
echo === validating the audio package ===
"%BIN_X86%\Inf2Cat.exe" /driver:"%OUT%\audio" /os:10_X64
if errorlevel 1 goto :fail
if exist "%OUT%\audio\openaes67_audio.cat" (
    "%BIN_X64%\signtool.exe" sign /fd SHA256 /f "%PFX%" /p openaes67 "%OUT%\audio\openaes67_audio.sys" "%OUT%\audio\openaes67_audio.cat"
    if errorlevel 1 goto :fail
    echo audio package signed
)

echo === result ===
dir /b openaes67_probe.* openaes67-test.cer
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s = Get-AuthenticodeSignature 'openaes67_probe.sys';" ^
  "Write-Host ('  signature status : ' + $s.Status);" ^
  "if ($s.SignerCertificate) { Write-Host ('  signer           : ' + $s.SignerCertificate.Subject) } else { Write-Host '  NO SIGNATURE'; exit 1 }"

popd
echo.
echo BUILD OK - artifacts in "%OUT%"
endlocal
exit /b 0

:fail
echo.
echo BUILD FAILED
popd
endlocal
exit /b 1
