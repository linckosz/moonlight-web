#!/usr/bin/env bash
# Generate compile_commands.json for clang-tidy from the qmake-produced
# Makefile.Release. qmake+MSVC does not emit a compilation database natively.
#
# Uses clang-cl driver mode so Clang accepts the MSVC-style flags; relies on
# clang-cl's automatic MSVC/Windows-SDK detection for system headers (only the
# project + Qt include paths come from the Makefile).
#
# Usage: run from backend/ after a build (Makefile.Release must exist):
#   bash scripts/gen_compile_commands.sh
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd -W 2>/dev/null || pwd)"
MK="Makefile.Release"
[ -f "$MK" ] || { echo "ERROR: $MK not found — build first."; exit 1; }

# Pull DEFINES / INCPATH straight from the Makefile so we stay in sync.
strip_var() { grep -m1 "^$1 " "$MK" | sed "s/^$1 *= *//"; }
DEFINES="$(strip_var DEFINES)"
# Unescape the Makefile's shell-level \" around string macros: passed via the
# JSON "arguments" array there is no shell, so they must be real quotes.
DEFINES="${DEFINES//\\\"/\"}"
INCPATH="$(strip_var INCPATH | tr '\\' '/')"

# Parse-relevant flags only (no codegen flags needed for static analysis).
# _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH: bundled clang-tidy is LLVM 19 but a
# newer MSVC STL may be auto-detected (STL1000 static_assert) — bypass the check.
BASE_FLAGS=(/std:c++17 /EHsc /permissive- /Zc:__cplusplus /Zc:wchar_t
    -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)

out="compile_commands.json"
{
  echo "["
  first=1
  while IFS= read -r f; do
    abs="$ROOT/$f"
    args='"clang-cl"'
    for x in "${BASE_FLAGS[@]}" $DEFINES $INCPATH; do
      esc=${x//\\/\\\\}; esc=${esc//\"/\\\"}
      args="$args, \"$esc\""
    done
    args="$args, \"$abs\""
    [ $first -eq 1 ] && first=0 || echo ","
    printf '  {"directory": "%s", "file": "%s", "arguments": [%s]}' "$ROOT" "$abs" "$args"
  done < <(find src -type f \( -name '*.cpp' \) -not -path '*/build/*')
  echo ""
  echo "]"
} > "$out"

echo "Wrote $out ($(grep -c '"file":' "$out") entries)"
