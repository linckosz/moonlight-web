@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d D:\Code\moonlight-web-deepseek\build\Desktop_Qt_6_11_0_MSVC2022_64bit-Debug
C:\Qt\Tools\QtCreator\bin\jom\jom.exe %*
