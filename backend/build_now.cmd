@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] VS 2022 not found
    exit /b 1
)
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
cd /d "d:\Code\moonlight-web-deepseek\backend"
rmdir /s /q build\release 2>nul
mkdir build\release
echo [QMAKE] Running qmake...
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)
echo [NMAKE] Running nmake...
nmake /f Makefile.Release
if errorlevel 1 (
    echo [ERROR] nmake failed
    exit /b 1
)
echo [OK] Build complete