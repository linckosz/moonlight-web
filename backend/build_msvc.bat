@echo off
REM ============================================================================
REM  MoonlightWeb - one-shot Windows build (MSVC 2022 + Qt 6.11, Ninja, Release)
REM
REM  Run from the repo root:
REM      cmd /c backend\build_msvc.bat
REM
REM  Output binary:
REM      build\MoonlightWeb.exe        (then open https://localhost)
REM
REM  Optional overrides (set before running):
REM      set QTDIR=C:\Qt\6.11.0\msvc2022_64    Qt kit, if not auto-detected
REM      set BUILD_DIR=build                   build directory (default: build)
REM ============================================================================
setlocal enabledelayedexpansion

REM ---- Repo paths (this script lives in <root>\backend) ----
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.." >nul
set "ROOT=%CD%"
popd >nul
if not defined BUILD_DIR set "BUILD_DIR=%ROOT%\build"

REM ---- Init git submodules on first run (moonlight-common-c, libdatachannel, ...) ----
if not exist "%ROOT%\backend\third_party\libdatachannel\CMakeLists.txt" (
    echo [INFO] Fetching git submodules...
    git -C "%ROOT%" submodule update --init --recursive || (
        echo [ERROR] git submodule update failed
        exit /b 1
    )
)

REM ---- Enter the MSVC x64 developer environment (skip if already in one) ----
if not defined VSINSTALLDIR (
    set "VSPATH="
    set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!_VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!_VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
    )
    if not defined VSPATH set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || (
        echo [ERROR] Visual Studio 2022 with the C++ toolset was not found.
        echo         Install "Desktop development with C++" ^(MSVC v143, Windows SDK,
        echo         and the "C++ CMake tools for Windows" component^).
        exit /b 1
    )
)

REM ---- Locate Qt (QTDIR / CMAKE_PREFIX_PATH override, else default kit) ----
if not defined QTDIR (
    if defined CMAKE_PREFIX_PATH (
        set "QTDIR=%CMAKE_PREFIX_PATH%"
    ) else (
        for %%v in (6.11.0 6.11.1 6.11.2 6.11.3) do (
            if exist "C:\Qt\%%v\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QTDIR=C:\Qt\%%v\msvc2022_64"
        )
    )
)
if not defined QTDIR (
    echo [ERROR] Qt 6.11 ^(MSVC 2022 64-bit^) was not found.
    echo         Install it, or point QTDIR at your kit, e.g.:
    echo             set QTDIR=C:\Qt\6.11.0\msvc2022_64
    exit /b 1
)
echo [INFO] Qt kit : %QTDIR%

REM ---- Make sure Ninja is on PATH (use the copy bundled with VS if needed) ----
where ninja >nul 2>&1 || set "PATH=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where ninja >nul 2>&1 || (
    echo [ERROR] Ninja was not found. Install the "C++ CMake tools for Windows"
    echo         Visual Studio component ^(or add ninja to PATH^).
    exit /b 1
)

REM ---- Configure + build ----
echo [BUILD] Configuring (Ninja, Release)...
cmake -S "%ROOT%\backend" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QTDIR%"
if errorlevel 1 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

echo [BUILD] Compiling...
cmake --build "%BUILD_DIR%" -j
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

echo.
echo [OK] Built: %BUILD_DIR%\MoonlightWeb.exe
echo      Run it, then open https://localhost
endlocal
