@echo off
REM MoonlightWeb UPNPClient Unit Tests — Build & Run
REM
REM Usage:
REM   run_upnp_tests              — build and run fallback tests only
REM   run_upnp_tests --upnp       — build with miniupnpc and run full E2E tests
REM   run_upnp_tests --clean      — clean build artifacts
REM
REM Prerequisites:
REM   - Qt 6.x in PATH (qmake, nmake)
REM   - Microsoft Visual C++ 2022 (cl, link, lib)
REM   - For --upnp: miniupnpc must be built (run ..\build_miniupnpc.bat first)

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build
set TEST_EXE=%BUILD_DIR%\release\run_tests.exe
set CLEAN_ONLY=0
set UPNP_FLAG=

if /I "%1"=="--clean" set CLEAN_ONLY=1
if /I "%1"=="--upnp" set UPNP_FLAG=--upnp

if %CLEAN_ONLY%==1 (
    echo [tests] Cleaning build artifacts...
    if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
    echo [tests] Clean complete.
    exit /b 0
)

REM Step 1: Build miniupnpc if --upnp was requested
if defined UPNP_FLAG (
    if not exist "%SCRIPT_DIR%..\third_party\miniupnpc\lib\miniupnpc.lib" (
        echo [tests] Building miniupnpc first...
        call "%SCRIPT_DIR%..\build_miniupnpc.bat"
        if errorlevel 1 (
            echo [tests] ERROR: miniupnpc build failed
            exit /b 1
        )
    ) else (
        echo [tests] miniupnpc already built
    )
)

REM Step 2: Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

REM Step 3: Generate Makefile with qmake
echo [tests] Running qmake...
qmake "%SCRIPT_DIR%tests.pro" -o Makefile
if errorlevel 1 (
    echo [tests] ERROR: qmake failed
    exit /b 1
)

REM Step 4: Build with nmake
echo [tests] Building...
nmake /A
if errorlevel 1 (
    echo [tests] ERROR: Build failed
    exit /b 1
)

REM Step 5: Run tests
echo.
echo ============================================
echo  Running UPNPClient unit tests...
echo ============================================
if defined UPNP_FLAG (
    "%TEST_EXE%" --upnp
) else (
    "%TEST_EXE%"
)

if errorlevel 1 (
    echo [tests] Some tests FAILED
) else (
    echo [tests] All tests PASSED
)

echo.
echo ============================================
echo  Tests complete.
echo ============================================

exit /b %errorlevel%
