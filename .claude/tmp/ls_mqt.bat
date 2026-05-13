@echo off
dir /s /b "D:\Code\moonlight-qt\app\src\*.cpp" | findstr /i "upnp igd portmap"
echo ---
dir /s /b "D:\Code\moonlight-qt\app\src\*.h" 2>nul
echo DIRDONE
dir /b "D:\Code\moonlight-qt\app"
