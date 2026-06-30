@echo off
REM ===========================================================================
REM  MoonlightWeb — install as an auto-restarting Windows service via NSSM.
REM
REM  The app is a normal GUI/tray executable, not an SCM-aware service, so we
REM  wrap it with NSSM (https://nssm.cc). Put nssm.exe on PATH or next to this
REM  script, then run this file from an elevated (Administrator) prompt.
REM
REM  Restart policy: NSSM relaunches the process on a crash, but a clean exit
REM  (code 0 = user quit via tray / Stop) is treated as a real stop and is NOT
REM  relaunched.
REM  (Note: a Windows service runs in session 0 with no desktop, so the tray
REM   icon will not appear while running this way.)
REM ===========================================================================
setlocal
set SVC=MoonlightWeb
set BIN=%~dp0MoonlightWeb.exe

where nssm >nul 2>&1
if errorlevel 1 (
    if exist "%~dp0nssm.exe" (
        set "NSSM=%~dp0nssm.exe"
    ) else (
        echo [ERROR] nssm.exe not found on PATH or next to this script.
        echo         Download it from https://nssm.cc and retry.
        exit /b 1
    )
) else (
    set "NSSM=nssm"
)

if not exist "%BIN%" (
    echo [ERROR] %BIN% not found. Run this from the install folder.
    exit /b 1
)

"%NSSM%" install %SVC% "%BIN%"
"%NSSM%" set %SVC% AppDirectory "%~dp0"
"%NSSM%" set %SVC% DisplayName "MoonlightWeb streaming server"
"%NSSM%" set %SVC% Description "Browser-based Sunshine streaming server"
"%NSSM%" set %SVC% Start SERVICE_AUTO_START

REM Mark this as a supervised launch: on an auto-restart the instance must NOT
REM steal a UPnP port mapping owned by another device (only a manual launch wins).
"%NSSM%" set %SVC% AppEnvironmentExtra MW_SERVICE=1

REM Restart on any unexpected exit, but exit code 0 (clean quit) = do NOT restart.
"%NSSM%" set %SVC% AppExit Default Restart
"%NSSM%" set %SVC% AppExit 0 Exit
"%NSSM%" set %SVC% AppRestartDelay 2000
REM Crash-loop guard: a process that dies < 5 s after start is throttled.
"%NSSM%" set %SVC% AppThrottle 5000

"%NSSM%" start %SVC%
echo.
echo [OK] Service "%SVC%" installed and started.
endlocal
