@echo off
set PATH=C:\Qt\6.11.0\msvc2022_64\bin;C:\Qt\6.11.0\msvc2022_64\plugins\tls;%PATH%
cd /d "%~dp0"
mw-server.exe
