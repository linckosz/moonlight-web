call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
cd /d "d:\Code\moonlight-web-deepseek\backend"
if not exist build\release mkdir build\release
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG-=debug_and_release 2>&1
if errorlevel 1 exit /b 1
nmake /f Makefile.Release 2>&1