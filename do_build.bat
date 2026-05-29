@echo off
setlocal
cd /d D:\Code\moonlight-web-deepseek\backend
call build_msvc.bat 2>&1
echo BUILD_RESULT=%ERRORLEVEL%
