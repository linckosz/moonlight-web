@echo off
setlocal enabledelayedexpansion

REM ---- Detect VS 2022 ----
if not defined VSINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || (
        echo [ERROR] Visual Studio 2022 not found
        exit /b 1
    )
)

REM ---- Qt 6.11 paths ----
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%

cd /d "d:\Code\moonlight-web-deepseek\backend"

REM ---- qmake ----
echo [BUILD] Running qmake...
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)

REM ---- Patch Makefile ----
echo [BUILD] Patching Makefile...
powershell -NoProfile -ExecutionPolicy Bypass -File "patch_makefile.ps1"

REM ---- nmake ----
echo [BUILD] Running nmake...
nmake /f Makefile.Release
if errorlevel 1 (
    echo [ERROR] nmake failed
    exit /b 1
)

echo [OK] Build successful
