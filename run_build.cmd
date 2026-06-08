@echo off
cd /d d:\Code\moonlight-web-deepseek\backend
call build_msvc.bat
if errorlevel 1 (
    echo BUILD FAILED
    pause
    exit /b 1
)
echo BUILD SUCCESS
pause