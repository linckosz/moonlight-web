@echo off
cd /d D:\Code\moonlight-web-deepseek
echo === Git log ===
git log --oneline -10
echo.
echo === Diff between commits ===
git diff 5f0f588..a52f2cf --name-only
