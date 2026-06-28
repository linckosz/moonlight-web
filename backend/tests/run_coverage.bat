@echo off
REM ============================================================================
REM Moonlight-Web - Backend TNR runner + coverage gate.
REM
REM Builds the unit-test runner (CMake + Ninja, MSVC x64 + Qt 6.11), runs it for
REM the pass/fail gate, then re-runs it under OpenCppCoverage to produce an HTML
REM report + a Cobertura XML and enforces the 70% line-coverage gate over the
REM in-scope sources.
REM
REM OpenCppCoverage is optional: without it the tests still run (pass/fail gate),
REM only the coverage percentage is skipped. Install once with:
REM   winget install OpenCppCoverage.OpenCppCoverage
REM ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM ---- Visual Studio 2022 x64 environment (Ninja needs cl on PATH) ----
if not defined VSINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

REM ---- Qt 6.11 ----
if not defined QTDIR set "QTDIR=C:\Qt\6.11.0\msvc2022_64"
set "PATH=%QTDIR%\bin;%PATH%"

REM ---- Configure + build (shadow build under tests\build) ----
rmdir /s /q build 2>nul
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%"
if errorlevel 1 goto cfg_fail
cmake --build "%~dp0build" -j
if errorlevel 1 goto build_fail

set "RUNNER=%~dp0build\run_tests.exe"
if not exist "%RUNNER%" goto no_exe

REM ---- 1) Pass/fail gate: run the suite directly (reliable exit code) ----
"%RUNNER%"
if errorlevel 1 goto tests_fail

REM ---- 2) Coverage report (optional). OpenCppCoverage's own exit code is
REM        unreliable, so we ignore it and gate on the parsed XML instead. ----
if exist "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe" set "PATH=C:\Program Files\OpenCppCoverage;%PATH%"
where OpenCppCoverage >nul 2>nul
if errorlevel 1 goto no_coverage

del cov.xml 2>nul
REM Relative path filters: the checkout dir name differs between local and CI
REM (moonlight-web-deepseek vs moonlight-web), so match on backend\... only.
OpenCppCoverage --quiet --sources backend\src --excluded_sources backend\third_party --excluded_sources backend\tests --export_type cobertura:cov.xml --export_type html:covhtml -- "%RUNNER%" >nul 2>&1
if not exist cov.xml goto no_report

REM ---- 3) Coverage gate ----
powershell -NoProfile -ExecutionPolicy Bypass -File check_coverage.ps1 -CoverageXml cov.xml -Threshold 70
exit /b %errorlevel%

:cfg_fail
echo [ERROR] CMake configure failed
exit /b 1
:build_fail
echo [ERROR] CMake build failed
exit /b 1
:no_exe
echo [ERROR] run_tests.exe not found after build
exit /b 1
:tests_fail
echo [FAIL] Backend tests reported failures
exit /b 1
:no_coverage
echo [WARN] OpenCppCoverage not installed - coverage percentage skipped (tests passed).
echo [WARN] Install: winget install OpenCppCoverage.OpenCppCoverage
exit /b 0
:no_report
echo [WARN] OpenCppCoverage produced no report - coverage percentage skipped.
exit /b 0
