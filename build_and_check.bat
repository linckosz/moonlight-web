@echo off
cd /d D:\Code\moonlight-web-deepseek\backend
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo QMAKE_FAILED
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Code\moonlight-web-deepseek\backend\patch_makefile.ps1" 2>&1
nmake /f Makefile.Release 2>&1
echo BUILD_EXIT=%ERRORLEVEL%
