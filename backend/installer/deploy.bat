@echo off
REM MoonlightWeb — Windows deployment script
REM Stages all runtime files into deploy/MoonlightWeb/ for MSI packaging.
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
set DEPLOY_DIR=%~dp0MoonlightWeb
set FRONTEND_DIR=%PROJECT_DIR%\..\frontend

echo === MoonlightWeb Deploy ===
echo Build:    %BUILD_DIR%
echo Deploy:   %DEPLOY_DIR%
echo Frontend: %FRONTEND_DIR%

REM Verify build exists
if not exist "%BUILD_DIR%\MoonlightWeb.exe" (
    echo ERROR: MoonlightWeb.exe not found. Run build_msvc.bat first.
    exit /b 1
)

REM Clean deploy directory
if exist "%DEPLOY_DIR%" rmdir /s /q "%DEPLOY_DIR%"
mkdir "%DEPLOY_DIR%"

REM Step 1: Copy binary
echo [1/4] Copying binary...
copy "%BUILD_DIR%\MoonlightWeb.exe" "%DEPLOY_DIR%\" > nul

REM Step 2: Run windeployqt
echo [2/4] Running windeployqt...
windeployqt --release "%DEPLOY_DIR%\MoonlightWeb.exe"
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

REM Step 3b: Force-deploy the Qt OpenSSL TLS backend plugin. windeployqt ships
REM only schannel/certonly; without qopensslbackend.dll the OpenSSL backend is
REM unavailable and Qt falls back to Schannel, which cannot load the public ACME
REM PEM key (browser then served the self-signed cert -> CN/SAN mismatch). See
REM the QSslSocket::setActiveBackend("openssl") call in main.cpp.
echo [3b] Copying Qt OpenSSL TLS plugin...
for /f "delims=" %%I in ('where windeployqt') do set "WDQ_DIR=%%~dpI"
if exist "%WDQ_DIR%..\plugins\tls\qopensslbackend.dll" (
    if not exist "%DEPLOY_DIR%\tls" mkdir "%DEPLOY_DIR%\tls"
    copy "%WDQ_DIR%..\plugins\tls\qopensslbackend.dll" "%DEPLOY_DIR%\tls\" > nul
    echo   + qopensslbackend.dll
) else (
    echo WARNING: qopensslbackend.dll not found near windeployqt
)

REM Step 4: Copy frontend directory
echo [4/4] Copying frontend...
xcopy /e /i /q "%FRONTEND_DIR%" "%DEPLOY_DIR%\frontend\" > nul

echo === Deploy complete: %DEPLOY_DIR% ===
echo.
echo To build the installer (Inno Setup):
echo   iscc installer\moonlightweb.iss /DSourceDir=%DEPLOY_DIR%
echo.
echo To run directly:
echo   %DEPLOY_DIR%\MoonlightWeb.exe

endlocal
