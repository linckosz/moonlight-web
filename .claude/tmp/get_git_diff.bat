@echo off
cd /d d:\Code\moonlight-web-deepseek
echo === style.css ===
git diff HEAD -- frontend/css/style.css
echo === index.html ===
git diff HEAD -- frontend/index.html
echo === app.js ===
git diff HEAD -- frontend/js/app.js
echo === stream.css ===
git diff HEAD -- frontend/css/stream.css
