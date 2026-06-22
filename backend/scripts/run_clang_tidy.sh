#!/usr/bin/env bash
# Run clang-tidy over the backend sources using a freshly generated
# compile_commands.json (clang-cl driver mode — see gen_compile_commands.sh).
#
# Usage (from backend/):
#   bash scripts/run_clang_tidy.sh            # all compiled sources
#   bash scripts/run_clang_tidy.sh src/foo.cpp [...]   # specific files
set -uo pipefail

cd "$(dirname "$0")/.."

# clang-tidy ships with Visual Studio's bundled LLVM.
CT="${CLANG_TIDY:-C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-tidy.exe}"
[ -x "$CT" ] || { echo "ERROR: clang-tidy not found at $CT (set CLANG_TIDY=...)"; exit 1; }

bash scripts/gen_compile_commands.sh >/dev/null

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(find src -name '*.cpp' -not -path '*/build/*')
fi

"$CT" -p . --quiet "${files[@]}"
