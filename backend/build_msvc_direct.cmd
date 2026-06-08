@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
cd /d "d:\Code\moonlight-web-deepseek\backend"
rmdir /s /q build\release 2>nul
mkdir build\release
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
nmake /f Makefile.Release