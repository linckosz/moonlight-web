#!/bin/bash
cd "/d/Code/moonlight-web-deepseek/backend" || exit 1
cmd //c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && qmake6 -o Makefile.Release backend.pro -spec win32-msvc CONFIG+=release && nmake /f Makefile.Release" 2>&1 | tail -60
echo "DONE"
