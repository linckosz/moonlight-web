@echo off
REM Moonlight-Web — Windows deployment script
REM Stages all runtime files into deploy/Moonlight-Web/ for MSI packaging.
REM
REM Usage:
REM   cd backend
REM   build_msvc.bat
REM   installer\deploy.bat
REM
REM Requires:
REM   - Qt 6.x bin\ in PATH (for windeployqt)
REM   - MSVC 2022 environment initialized

setlocal

set PROJECT_DIR=%~dp0..
set BUILD_DIR=%PROJECT_DIR%\build\release
set DEPLOY_DIR=%~dp0Moonlight-Web
set FRONTEND_DIR=%PROJECT_DIR%\..\frontend

echo === Moonlight-Web Deploy ===
echo Build:    %BUILD_DIR%
echo Deploy:   %DEPLOY_DIR%
echo Frontend: %FRONTEND_DIR%

REM Verify build exists
if not exist "%BUILD_DIR%\moonlight-web.exe" (
    echo ERROR: moonlight-web.exe not found. Run build_msvc.bat first.
    exit /b 1
)

REM Clean deploy directory
if exist "%DEPLOY_DIR%" rmdir /s /q "%DEPLOY_DIR%"
mkdir "%DEPLOY_DIR%"

REM Step 1: Copy binary
echo [1/4] Copying binary...
copy "%BUILD_DIR%\moonlight-web.exe" "%DEPLOY_DIR%\" > nul

REM Step 2: Run windeployqt
echo [2/4] Running windeployqt...
windeployqt --release "%DEPLOY_DIR%\moonlight-web.exe"
if %ERRORLEVEL% neq 0 (
    echo WARNING: windeployqt returned error %ERRORLEVEL%
)

REM Step 3: Copy OpenSSL DLLs
echo [3/4] Copying OpenSSL DLLs...
if exist "%BUILD_DIR%\libcrypto-3-x64.dll" (
    copy "%BUILD_DIR%\libcrypto-3-x64.dll" "%DEPLOY_DIR%\" > nul
) else (
    echo WARNING: libcrypto-3-x64.dll not found in build dir
)
if exist "%BUILD_DIR%\libssl-3-x64.dll" (
    copy "%BUILD_DIR%\libssl-3-x64.dll" "%DEPLOY_DIR%\" > nul
) else (
    echo WARNING: libssl-3-x64.dll not found in build dir
)

REM Step 4: Copy frontend directory
echo [4/4] Copying frontend...
xcopy /e /i /q "%FRONTEND_DIR%" "%DEPLOY_DIR%\frontend\" > nul

echo === Deploy complete: %DEPLOY_DIR% ===
echo.
echo To build MSI:
echo   installer\build_msi.bat
echo.
echo To run directly:
echo   %DEPLOY_DIR%\moonlight-web.exe

endlocal
