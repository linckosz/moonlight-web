@echo off
echo === References to AppSettings::domain() or .domain() ===
findstr /s /n "\.domain()" d:\Code\moonlight-web-deepseek\backend\src\*.cpp d:\Code\moonlight-web-deepseek\backend\src\*.h 2>nul | findstr /v "third_party" | findstr /v ".claude"
echo === Direct "domain" JSON key access ===
findstr /s /n """domain""" d:\Code\moonlight-web-deepseek\backend\src\*.cpp d:\Code\moonlight-web-deepseek\backend\src\*.h 2>nul | findstr /v "third_party" | findstr /v ".claude"
echo === References to setDomain ===
findstr /s /n "setDomain" d:\Code\moonlight-web-deepseek\backend\src\*.cpp d:\Code\moonlight-web-deepseek\backend\src\*.h 2>nul | findstr /v "third_party" | findstr /v ".claude"
