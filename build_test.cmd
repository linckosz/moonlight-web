@echo off
cd /d D:\Code\moonlight-web-deepseek\backend
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
cd /d D:\Code\moonlight-web-deepseek\backend
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Code\moonlight-web-deepseek\backend\patch_makefile.ps1"
nmake /f Makefile.Release
echo EXIT_CODE=%ERRORLEVEL%
pause
