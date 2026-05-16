@echo off
REM Build miniupnpc as static lib for MSVC from git submodule
REM Requires: git submodule init && git submodule update
REM Output:   third_party/miniupnp/build/lib/miniupnpc.lib
REM           third_party/miniupnp/build/include/miniupnpc/

setlocal enabledelayedexpansion

REM ---- Detect VS 2022 ----
if not defined VSINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || (
        echo [ERROR] Visual Studio 2022 not found
        exit /b 1
    )
)

set SUBMODULE_DIR=%~dp0third_party\miniupnp\miniupnpc
set SRC_DIR=%SUBMODULE_DIR%\src
set HEADER_DIR=%SUBMODULE_DIR%\include
set BUILD_DIR=%~dp0third_party\miniupnp\build
set OUT_DIR=%BUILD_DIR%\lib
set INC_DIR=%BUILD_DIR%\include\miniupnpc

echo [miniupnpc] Building static library from submodule...
echo [miniupnpc]   Source: %SRC_DIR%
echo [miniupnpc]   Output: %OUT_DIR%

REM Step 1: Verify submodule is present
if not exist "%SRC_DIR%\miniupnpc.c" (
    echo [miniupnpc] ERROR: miniupnpc sources not found
    echo [miniupnpc] Run: git submodule init ^&^& git submodule update
    exit /b 1
)

REM Step 2: Create output directories
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if not exist "%INC_DIR%" mkdir "%INC_DIR%"

REM Step 3: Copy public headers to include dir
echo [miniupnpc] Copying headers...
copy /Y "%HEADER_DIR%\*.h" "%INC_DIR%\" >nul
REM Also copy internal headers needed for compilation
copy /Y "%SRC_DIR%\win32_snprintf.h" "%INC_DIR%\" >nul

REM Step 4: Generate miniupnpcstrings.h (required by minisoap.c)
echo [miniupnpc] Generating miniupnpcstrings.h...
(
echo #ifndef MINIUPNPCSTRINGS_H_INCLUDED
echo #define MINIUPNPCSTRINGS_H_INCLUDED
echo #define OS_STRING "Windows"
echo #define MINIUPNPC_VERSION_STRING "2.2.8"
echo #define UPNP_VERSION_MAJOR 1
echo #define UPNP_VERSION_MINOR 1
echo #define UPNP_VERSION_STRING "UPnP/1.1"
echo #endif
) > "%INC_DIR%\miniupnpcstrings.h"

REM Step 5: Compile source files
echo [miniupnpc] Compiling source files...

set CFLAGS=/c /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DMINIUPNP_STATICLIB /I"%SRC_DIR%" /I"%INC_DIR%"

for %%f in (
    igd_desc_parse
    miniupnpc
    minixml
    minisoap
    minissdpc
    miniwget
    upnpcommands
    upnpdev
    upnpreplyparse
    upnperrors
    connecthostport
    portlistingparse
    receivedata
    addr_is_reserved
) do (
    echo [miniupnpc] Compiling %%f.c...
    cl %CFLAGS% /Fo"%OUT_DIR%\%%f.obj" "%SRC_DIR%\%%f.c"
    if errorlevel 1 goto :error
)

REM Step 6: Create static library
echo [miniupnpc] Creating library...
lib /OUT:"%OUT_DIR%\miniupnpc.lib" ^
    "%OUT_DIR%\igd_desc_parse.obj" ^
    "%OUT_DIR%\miniupnpc.obj" ^
    "%OUT_DIR%\minixml.obj" ^
    "%OUT_DIR%\minisoap.obj" ^
    "%OUT_DIR%\minissdpc.obj" ^
    "%OUT_DIR%\miniwget.obj" ^
    "%OUT_DIR%\upnpcommands.obj" ^
    "%OUT_DIR%\upnpdev.obj" ^
    "%OUT_DIR%\upnpreplyparse.obj" ^
    "%OUT_DIR%\upnperrors.obj" ^
    "%OUT_DIR%\connecthostport.obj" ^
    "%OUT_DIR%\portlistingparse.obj" ^
    "%OUT_DIR%\receivedata.obj" ^
    "%OUT_DIR%\addr_is_reserved.obj"

if errorlevel 1 goto :error

echo [miniupnpc] Library built successfully at %OUT_DIR%\miniupnpc.lib
goto :eof

:error
echo [miniupnpc] BUILD FAILED with error code %errorlevel%
exit /b %errorlevel%
