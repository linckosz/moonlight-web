@echo off
for /r "D:\Code\moonlight-web-stream\web\stream" %%f in (*.ts) do @echo %%f
echo ---
for /r "D:\Code\moonlight-web-stream\web\stream" %%f in (*.js) do @echo %%f
