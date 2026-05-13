@echo off
cd /d d:\Code\moonlight-web-deepseek\backend
call build_msvc.bat
echo.
echo Exit code: %errorlevel%
pause
