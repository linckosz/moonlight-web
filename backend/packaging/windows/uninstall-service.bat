@echo off
REM Remove the Moonlight-Web NSSM service. Run from an elevated prompt.
setlocal
set SVC=Moonlight-Web

where nssm >nul 2>&1
if errorlevel 1 (
    if exist "%~dp0nssm.exe" ( set "NSSM=%~dp0nssm.exe" ) else (
        echo [ERROR] nssm.exe not found. & exit /b 1
    )
) else (
    set "NSSM=nssm"
)

"%NSSM%" stop %SVC%
"%NSSM%" remove %SVC% confirm
echo [OK] Service "%SVC%" removed.
endlocal
