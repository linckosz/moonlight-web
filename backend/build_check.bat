@echo off
cd /d "D:\Code\moonlight-web-deepseek\backend"

REM Run qmake to regenerate makefile and moc files
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set QTDIR=C:\Qt\6.11.0\msvc2022_64
set PATH=%QTDIR%\bin;%PATH%

echo === Running qmake ===
qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)

echo === Running nmake ===
nmake /f Makefile.Release
if errorlevel 1 (
    echo [ERROR] nmake failed
    exit /b 1
)

echo [OK] Build complete
