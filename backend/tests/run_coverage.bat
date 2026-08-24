@echo off
REM ============================================================================
REM MoonlightWeb - Backend TNR runner + coverage gate.
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

REM ---- MSVC x64 environment (Ninja needs cl on PATH) ----
REM  Detected the same way as backend\build_msvc.bat rather than hard-coded: the
REM  old "Visual Studio\2022\Community" path matched exactly one machine, and on
REM  every other one this script died in CMake with "compiler not set" instead of
REM  saying what was missing. This suite is the release gate; it has to run.
if not defined VSINSTALLDIR (
    set "VSPATH="
    set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!_VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!_VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
    )
    if not defined VSPATH set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    if !errorlevel! neq 0 goto vs_fail
)

REM ---- Qt: highest 6.x MSVC kit under C:\Qt, unless QTDIR overrides ----
if not defined QTDIR (
    for /d %%d in ("C:\Qt\6.*") do (
        if exist "%%d\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QTDIR=%%d\msvc2022_64"
    )
)
if not defined QTDIR goto qt_fail
echo [INFO] Qt kit : !QTDIR!
set "PATH=!QTDIR!\bin;%PATH%"

REM ---- Configure + build (shadow build under tests\build) ----
rmdir /s /q build 2>nul
REM `neq 0`, not `if errorlevel 1`: the latter is a signed >= test and cmake
REM returns -1 when the link step fails, which would fall through to the run.
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTDIR%"
if !errorlevel! neq 0 goto cfg_fail
cmake --build "%~dp0build" -j
if !errorlevel! neq 0 goto build_fail

set "RUNNER=%~dp0build\run_tests.exe"
if not exist "%RUNNER%" goto no_exe

REM ---- 1) Pass/fail gate: run the suite directly (reliable exit code) ----
REM  `neq 0` also catches a crashed runner: an access violation exits with
REM  0xC0000005, which is negative and would pass an `if errorlevel 1` gate.
"%RUNNER%"
if !errorlevel! neq 0 goto tests_fail

REM ---- 2) Coverage report (optional). OpenCppCoverage's own exit code is
REM        unreliable, so we ignore it and gate on the parsed XML instead. ----
if exist "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe" set "PATH=C:\Program Files\OpenCppCoverage;%PATH%"
where OpenCppCoverage >nul 2>nul
if errorlevel 1 goto no_coverage

del cov.xml 2>nul
REM Relative path filters: the checkout dir name differs between local and CI
REM (e.g. moonlight-web vs moonlightweb), so match on backend\... only.
OpenCppCoverage --quiet --sources backend\src --excluded_sources backend\third_party --excluded_sources backend\tests --export_type cobertura:cov.xml --export_type html:covhtml -- "%RUNNER%" >nul 2>&1
if not exist cov.xml goto no_report

REM ---- 3) Coverage gate ----
powershell -NoProfile -ExecutionPolicy Bypass -File check_coverage.ps1 -CoverageXml cov.xml -Threshold 70
exit /b %errorlevel%

:vs_fail
echo [ERROR] No MSVC x64 toolset found. Install "Desktop development with C++"
echo         (MSVC v143 + Windows SDK), or run this from a Developer Prompt.
exit /b 1
:qt_fail
echo [ERROR] No Qt 6.x MSVC kit found under C:\Qt. Set QTDIR, e.g.:
echo             set QTDIR=C:\Qt\6.10.3\msvc2022_64
exit /b 1
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
