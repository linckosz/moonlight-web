# Enforces the licence boundary of mw-native-host.
#
# This module has to stay relicensable on its own (see LICENSE.md), which means
# it must never include a Qt header, a moonlight-common-c header, a header from
# backend/src/, or any GPL-licensed library. Those rules are easy to state and
# just as easy to break by reflex — an #include <QString> that "compiles fine"
# would quietly cost the whole commercialisation option.
#
# So the rule is mechanical: this script scans every source in the module and
# fails the build on the first violation, naming the file and the line.
#
# Run standalone with:
#   cmake -DMW_NATIVE_DIR=backend/native-host -P cmake/boundary_check.cmake

if(NOT DEFINED MW_NATIVE_DIR)
    message(FATAL_ERROR "boundary_check: MW_NATIVE_DIR is required")
endif()

file(GLOB_RECURSE _mw_sources
     "${MW_NATIVE_DIR}/include/*.h"
     "${MW_NATIVE_DIR}/src/*.h"
     "${MW_NATIVE_DIR}/src/*.cpp"
     "${MW_NATIVE_DIR}/src/*.mm"
     "${MW_NATIVE_DIR}/tests/*.cpp")

# Each entry: a regex matched against an #include line, plus why it is banned.
set(_mw_banned
    "Qt[A-Za-z]*/|<Q[A-Z][A-Za-z]*>|\"Q[A-Z][A-Za-z]*\""
        "Qt (LGPL/GPL, and would tie this module to the host application)"
    "Limelight\\.h|moonlight-common-c"
        "moonlight-common-c (GPL-3.0)"
    "libavcodec/|libavformat/|libavutil/|libswscale/"
        "FFmpeg (LGPL-2.1+, GPL when built with x264/x265)"
    "x264\\.h|x265\\.h"
        "x264/x265 (GPL-2.0)"
    "\\.\\./\\.\\./src/|backend/src/"
        "backend/src (the GPL host application)")

set(_mw_violations "")

foreach(_file ${_mw_sources})
    file(STRINGS "${_file}" _lines)
    set(_lineno 0)
    foreach(_line ${_lines})
        math(EXPR _lineno "${_lineno} + 1")
        if(NOT _line MATCHES "^[ \t]*#[ \t]*include")
            continue()
        endif()
        set(_i 0)
        list(LENGTH _mw_banned _banned_len)
        while(_i LESS _banned_len)
            math(EXPR _why_i "${_i} + 1")
            list(GET _mw_banned ${_i} _pattern)
            list(GET _mw_banned ${_why_i} _why)
            if(_line MATCHES "${_pattern}")
                list(APPEND _mw_violations
                     "  ${_file}:${_lineno}: ${_line}\n      -> forbidden: ${_why}")
            endif()
            math(EXPR _i "${_i} + 2")
        endwhile()
    endforeach()
endforeach()

if(_mw_violations)
    string(REPLACE ";" "\n" _mw_report "${_mw_violations}")
    message(FATAL_ERROR
        "\nmw-native-host licence boundary violated:\n\n${_mw_report}\n\n"
        "This module must stay free of Qt and of every GPL dependency so it can be\n"
        "relicensed on its own. See backend/native-host/LICENSE.md.\n")
endif()

list(LENGTH _mw_sources _mw_count)
message(STATUS "mw-native-host: licence boundary clean (${_mw_count} files scanned)")
