@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
cd /d "d:\Code\moonlight-web-deepseek\backend"
echo [1/3] qmake...
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
if errorlevel 1 exit /b 1
echo [2/3] patch_makefile...
powershell -NoProfile -ExecutionPolicy Bypass -File "patch_makefile.ps1"
echo [3/3] nmake...
nmake /f Makefile.Release
if errorlevel 1 exit /b 1
echo [OK] Build successful
