@echo off
REM ============================================
REM Build libdatachannel static library (MSVC 2022)
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

REM Check if libdatachannel already built
if exist "%PREFIX_DIR%\lib\datachannel.lib" (
    echo [OK] libdatachannel already built: %PREFIX_DIR%\lib\datachannel.lib
    exit /b 0
)

REM Check if submodule exists
if not exist "%LIBDATACHANNEL_DIR%\CMakeLists.txt" (
    echo [ERROR] libdatachannel submodule not found at %LIBDATACHANNEL_DIR%
    echo         Run: git submodule add https://github.com/paullouisageneau/libdatachannel.git backend/third_party/libdatachannel
    echo         Then: git submodule update --init --recursive
    exit /b 1
)

echo [BUILD] Configuring libdatachannel with CMake...
rmdir /s /q "%BUILD_DIR%" 2>nul
mkdir "%BUILD_DIR%"
mkdir "%PREFIX_DIR%"

cd /d "%BUILD_DIR%"

cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX="%PREFIX_DIR%" ^
    -DUSE_GNUTLS=OFF ^
    -DUSE_MBEDTLS=OFF ^
    -DNO_WEBSOCKET=ON ^
    -DNO_MEDIA=ON ^
    -DOPENSSL_ROOT_DIR="D:/Code/moonlight-web-deepseek/backend/libs/windows"

if errorlevel 1 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

echo [BUILD] Building libdatachannel...
cmake --build . --config Release
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

echo [BUILD] Installing libdatachannel to %PREFIX_DIR%...
cmake --install . --config Release
if errorlevel 1 (
    echo [ERROR] Install failed
    exit /b 1
)

echo [OK] libdatachannel built and installed to %PREFIX_DIR%
echo     Headers: %PREFIX_DIR%\include
echo     Library: %PREFIX_DIR%\lib\datachannel-static.lib
