@echo off
setlocal enabledelayedexpansion

if not defined VSINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || (
        echo [ERROR] Visual Studio 2022 not found
        exit /b 1
    )
)

set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%

cd /d "%~dp0"

REM Use existing Makefile if available, otherwise regenerate
if not exist "Makefile.Release" (
    qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
    if errorlevel 1 (
        echo [ERROR] qmake failed
        exit /b 1
    )
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0patch_makefile.ps1"
)

nmake /f Makefile.Release
if errorlevel 1 (
    echo [ERROR] nmake failed
    exit /b 1
)

echo [OK] Build complete
