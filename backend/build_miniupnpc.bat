@echo off
REM Build miniupnpc as static lib for MSVC
REM Clones miniupnpc from GitHub, compiles required source files,
REM and creates a static library in third_party/miniupnpc/lib/

setlocal enabledelayedexpansion

REM ---- Detect VS 2022 ----
if not defined VSINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || (
        echo [ERROR] Visual Studio 2022 not found
        exit /b 1
    )
)

set SRC_DIR=%~dp0third_party\miniupnpc\src
set OUT_DIR=%~dp0third_party\miniupnpc\lib
set INC_DIR=%~dp0third_party\miniupnpc\include\miniupnpc

echo [miniupnpc] Building static library...

REM Step 1: Clone or update miniupnpc source
if not exist "%SRC_DIR%\miniupnpc.c" (
    echo [miniupnpc] Downloading miniupnpc source...
    if not exist "%SRC_DIR%" mkdir "%SRC_DIR%"
    cd /d "%SRC_DIR%"
    REM Try git clone first; if no git, try curl
    git clone https://github.com/miniupnp/miniupnp.git tmp 2>nul
    if exist "tmp\miniupnpc\miniupnpc.c" (
        echo [miniupnpc] Source obtained via git
        REM Move files from subdirectory
        copy /Y "tmp\miniupnpc\*.c" "." >nul
        copy /Y "tmp\miniupnpc\*.h" "." >nul
        rmdir /S /Q tmp
    ) else (
        echo [miniupnpc] Git not available, trying curl with ZIP download...
        rmdir /S /Q tmp 2>nul
        powershell -Command "& {Invoke-WebRequest -Uri 'https://github.com/miniupnp/miniupnp/archive/refs/heads/master.zip' -OutFile '%SRC_DIR%\miniupnpc.zip'}"
        if exist "%SRC_DIR%\miniupnpc.zip" (
            cd /d "%SRC_DIR%"
            REM Use PowerShell to extract
            powershell -Command "& {Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::ExtractToDirectory('%SRC_DIR%\miniupnpc.zip', '%SRC_DIR%\tmp2')}"
            if exist "%SRC_DIR%\tmp2\miniupnp-master\miniupnpc" (
                copy /Y "%SRC_DIR%\tmp2\miniupnp-master\miniupnpc\*.c" "." >nul
                copy /Y "%SRC_DIR%\tmp2\miniupnp-master\miniupnpc\*.h" "." >nul
            )
            rmdir /S /Q "%SRC_DIR%\tmp2" 2>nul
            del "%SRC_DIR%\miniupnpc.zip"
        )
    )
    if not exist "%SRC_DIR%\miniupnpc.c" (
        echo [miniupnpc] ERROR: Failed to obtain miniupnpc source
        echo [miniupnpc] Please manually clone https://github.com/miniupnp/miniupnp
        echo [miniupnpc] and copy miniupnpc/*.c, miniupnpc/*.h into %SRC_DIR%
        exit /b 1
    )
    echo [miniupnpc] Source obtained successfully
)

REM Step 2: Create output directories
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if not exist "%INC_DIR%" mkdir "%INC_DIR%"

REM Step 3: Copy all headers to include dir
echo [miniupnpc] Copying headers...
copy /Y "%SRC_DIR%\*.h" "%INC_DIR%\" >nul

REM Step 4: Compile source files
echo [miniupnpc] Compiling source files...

set CFLAGS=/c /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DMINIUPNP_STATICLIB /I"%SRC_DIR%"

REM Compile all miniupnpc source files (from CMakeLists.txt MINIUPNPC_SOURCES)
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

REM Step 5: Create static library
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
echo [miniupnpc] To link: add -L%OUT_DIR% -lminiupnpc to your linker flags
goto :eof

:error
echo [miniupnpc] BUILD FAILED with error code %errorlevel%
exit /b %errorlevel%
