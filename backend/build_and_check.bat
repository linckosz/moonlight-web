@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] VS 2022 not found
    exit /b 1
)
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%
cd /d "%~dp0"

if not exist "third_party\libdatachannel\install\lib\datachannel.lib" (
    call build_libdatachannel.bat
    if errorlevel 1 exit /b 1
)

rmdir /s /q build\release 2>nul
mkdir build\release 2>nul

qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release CONFIG+=qtquickcompiler CONFIG-=debug_and_release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)

nmake /f Makefile.Release
set ERR=%errorlevel%
if exist Makefile.Release del /q Makefile.Release 2>nul
if %ERR% neq 0 (
    echo [ERROR] nmake failed
    exit /b 1
)
echo [OK] Build complete
exit /b 0
