@echo off
REM Minimal build step after qmake: run nmake
cd /d "%~dp0.."
nmake /f Makefile.Release
