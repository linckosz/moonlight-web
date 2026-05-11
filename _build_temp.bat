@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

REM Build qmdnsengine first
cd /d D:\Code\moonlight-web-deepseek\backend\build\third_party\qmdnsengine
C:\Qt\6.11.0\msvc2022_64\bin\qmake.exe D:\Code\moonlight-web-deepseek\backend\third_party\qmdnsengine\qmdnsengine.pro -spec win32-msvc CONFIG+=debug
if %errorlevel% neq 0 exit /b %errorlevel%
nmake
if %errorlevel% neq 0 exit /b %errorlevel%

REM Build the backend
cd /d D:\Code\moonlight-web-deepseek\backend\build
C:\Qt\6.11.0\msvc2022_64\bin\qmake.exe D:\Code\moonlight-web-deepseek\backend\backend.pro -spec win32-msvc CONFIG+=debug
if %errorlevel% neq 0 exit /b %errorlevel%
nmake
