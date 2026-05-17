@echo off
REM ============================================
REM Build libdatachannel static library (MSVC 2022)
REM Builds both Release and Debug configurations.
REM ============================================
setlocal enabledelayedexpansion

REM ---- Detect VS 2022 ----
if not defined VSINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || (
        echo [ERROR] Visual Studio 2022 not found
        exit /b 1
    )
)

set LIBDATACHANNEL_DIR=%~dp0third_party\libdatachannel
set BUILD_DIR=%LIBDATACHANNEL_DIR%\build
set PREFIX_DIR=%LIBDATACHANNEL_DIR%\install

REM ---- Check if both configs already built ----
if exist "%PREFIX_DIR%\lib\datachannel.lib" (
    if exist "%PREFIX_DIR%\debug\lib\datachannel.lib" (
        echo [OK] libdatachannel already built (Release + Debug^): %PREFIX_DIR%
        exit /b 0
    )
)

REM ---- Check if submodule exists ----
if not exist "%LIBDATACHANNEL_DIR%\CMakeLists.txt" (
    echo [ERROR] libdatachannel submodule not found at %LIBDATACHANNEL_DIR%
    echo         Run: git submodule add https://github.com/paullouisageneau/libdatachannel.git backend/third_party/libdatachannel
    echo         Then: git submodule update --init --recursive
    exit /b 1
)

echo [BUILD] Configuring libdatachannel with CMake for Release + Debug...

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DCMAKE_INSTALL_PREFIX="%PREFIX_DIR%" ^
    -DUSE_GNUTLS=OFF ^
    -DUSE_MBEDTLS=OFF ^
    -DNO_WEBSOCKET=ON ^
    -DNO_MEDIA=OFF ^
    -DNO_EXAMPLES=ON ^
    -DNO_TESTS=ON ^
    -DOPENSSL_ROOT_DIR="D:/Code/moonlight-web-deepseek/backend/libs/windows"

if errorlevel 1 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

REM ---- Build + install Release ----
echo [BUILD] Building libdatachannel (Release)...
cmake --build . --config Release
if errorlevel 1 (
    echo [ERROR] Release build failed
    exit /b 1
)
echo [BUILD] Installing libdatachannel (Release)...
cmake --install . --config Release

REM ---- Build + install Debug to install/debug/ ----
echo [BUILD] Building libdatachannel (Debug)...
cmake --build . --config Debug
if errorlevel 1 (
    echo [ERROR] Debug build failed
    exit /b 1
)

REM Debug install: use a separate prefix so Release + Debug don't collide
if not exist "%PREFIX_DIR%\debug" mkdir "%PREFIX_DIR%\debug"
echo [BUILD] Installing libdatachannel (Debug)...
cmake --install . --config Debug --prefix "%PREFIX_DIR%\debug"

echo [OK] libdatachannel built and installed
echo     Release: %PREFIX_DIR%\lib\datachannel.lib
echo     Debug:   %PREFIX_DIR%\debug\lib\datachannel.lib
