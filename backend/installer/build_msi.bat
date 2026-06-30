@echo off
REM MoonlightWeb — MSI build script
REM
REM Requires:
REM   - WiX Toolset v3.x (candle.exe + light.exe in PATH)
REM   - Deploy directory staged by deploy.bat
REM   - Visual Studio 2022 environment (for MSBuild/VC vars, unless candle/light are in PATH)

setlocal

set INSTALLER_DIR=%~dp0
set DEPLOY_DIR=%INSTALLER_DIR%MoonlightWeb
set BUILD_DIR=%INSTALLER_DIR%build
set OUTPUT_MSI=%INSTALLER_DIR%MoonlightWeb-0.1.0.msi

echo === MoonlightWeb MSI Build ===
echo Deploy:    %DEPLOY_DIR%
echo Output:    %OUTPUT_MSI%

REM Verify deploy directory exists
if not exist "%DEPLOY_DIR%\MoonlightWeb.exe" (
    echo ERROR: Deploy directory not staged. Run deploy.bat first.
    exit /b 1
)

REM Clean build directory
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

REM Step 1: Harvest deployed files with heat.exe
echo [1/3] Harvesting files with heat.exe...
heat dir "%DEPLOY_DIR%" ^
    -gg ^
    -cg DeployedFiles ^
    -dr APPLICATIONFOLDER ^
    -srd ^
    -var var.DeployDir ^
    -out "%BUILD_DIR%\deploy-files.wxs"
if %ERRORLEVEL% neq 0 (
    echo ERROR: heat.exe failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

REM Step 2: Compile with candle.exe
echo [2/3] Compiling...
candle "%INSTALLER_DIR%\moonlightweb.wxs" ^
    -dDeployDir="%DEPLOY_DIR%" ^
    -out "%BUILD_DIR%\moonlightweb.wixobj"
if %ERRORLEVEL% neq 0 (
    echo ERROR: candle.exe failed (moonlightweb.wxs) with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

candle "%BUILD_DIR%\deploy-files.wxs" ^
    -dDeployDir="%DEPLOY_DIR%" ^
    -out "%BUILD_DIR%\deploy-files.wixobj"
if %ERRORLEVEL% neq 0 (
    echo ERROR: candle.exe failed (deploy-files.wxs) with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

REM Step 3: Link with light.exe
echo [3/3] Linking MSI...
light "%BUILD_DIR%\moonlightweb.wixobj" "%BUILD_DIR%\deploy-files.wixobj" ^
    -ext WixUIExtension ^
    -out "%OUTPUT_MSI%"
if %ERRORLEVEL% neq 0 (
    echo ERROR: light.exe failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo === MSI built successfully: %OUTPUT_MSI% ===

endlocal
