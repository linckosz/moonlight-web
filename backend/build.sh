#!/usr/bin/env bash
# =============================================================================
#  MoonlightWeb - one-shot Linux / macOS build (CMake, Release).
#
#  Run from anywhere:
#      ./backend/build.sh
#
#  Output binary:
#      build/MoonlightWeb            (then open https://localhost)
#
#  Optional overrides (export before running):
#      CMAKE_PREFIX_PATH=<Qt kit>    Qt install, if not auto-detected
#      BUILD_DIR=build               build directory (default: build)
#
#  Toolchain: CMake >= 3.21, a C++17 compiler, and Qt 6.11 (Core, Network,
#  WebSockets, Widgets). Ninja is used automatically when available.
#    Debian/Ubuntu: sudo apt install build-essential cmake ninja-build pkg-config libssl-dev
#    macOS:         brew install cmake ninja openssl@3 pkg-config
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

# Init git submodules on first run (moonlight-common-c, libdatachannel, ...).
if [ ! -f "$ROOT/backend/third_party/libdatachannel/CMakeLists.txt" ]; then
  echo "[INFO] Fetching git submodules..."
  git -C "$ROOT" submodule update --init --recursive
fi

ARGS=(-S "$ROOT/backend" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release)
if command -v ninja >/dev/null 2>&1; then
  ARGS+=(-G Ninja)
fi
if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
  ARGS+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi

echo "[BUILD] Configuring (Release)..."
cmake "${ARGS[@]}"

echo "[BUILD] Compiling..."
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "[OK] Built: $BUILD_DIR/MoonlightWeb"
echo "     Run it, then open https://localhost"
