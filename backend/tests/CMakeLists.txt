# ============================================================================
# MoonlightWeb — Backend TNR (non-regression) unit tests.
#
# Standalone CMake project (replaces the old tests.pro): builds a console runner
# over the in-scope, deterministic logic units, linked against their REAL
# dependencies (no fragile stubs). Kept separate from the main backend build so
# it stays fast — it pulls only Qt Core/Network + OpenSSL, not the full WebRTC /
# UPnP / mDNS dependency tree.
#
# Build + run standalone (from an MSVC x64 environment):
#   cmake -S backend/tests -B backend/tests/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build backend/tests/build
#   backend/tests/build/run_tests.exe
#
# run_coverage.bat wraps this under OpenCppCoverage to enforce the 70% gate.
#
# NOTE: the legacy test_upnpclient.cpp (separate main, needs a LAN gateway) is
# intentionally not part of this runner.
# ============================================================================
cmake_minimum_required(VERSION 3.21)
project(moonlightweb-tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_src ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ── OpenSSL (InputCrypto / AuthManager) ─────────────────────────────────────
# Windows: prebuilt under ../libs/windows (committed). Linux/macOS: system pkg.
if(WIN32)
    set(_ssl "${_src}/libs/windows")
    set(OPENSSL_ROOT_DIR     "${_ssl}")
    set(OPENSSL_INCLUDE_DIR  "${_ssl}/include/x64" CACHE PATH "" FORCE)
    set(OPENSSL_SSL_LIBRARY    "${_ssl}/lib/x64/libssl.lib"    CACHE FILEPATH "" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${_ssl}/lib/x64/libcrypto.lib" CACHE FILEPATH "" FORCE)
elseif(APPLE)
    foreach(_brew /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS "${_brew}")
            set(OPENSSL_ROOT_DIR "${_brew}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenSSL 1.1 REQUIRED)

# ── Qt 6 ────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Network)
set(CMAKE_AUTOMOC ON)

# ── Test runner ─────────────────────────────────────────────────────────────
add_executable(run_tests
    # Test sources
    main.cpp
    test_input_encoder.cpp
    test_stream_config.cpp
    test_input_crypto.cpp
    test_rest_router.cpp
    test_app_settings.cpp
    test_auth_manager.cpp
    test_connection_guard.cpp
    test_static_files.cpp
    test_http_parser.cpp
    # In-scope production units (+ their real deps)
    ${_src}/src/streaming/InputEncoder.cpp
    ${_src}/src/streaming/StreamConfig.cpp
    ${_src}/src/streaming/InputCrypto.cpp
    ${_src}/src/server/RestRouter.cpp
    ${_src}/src/server/AppSettings.cpp
    ${_src}/src/server/AuthManager.cpp
    ${_src}/src/server/ConnectionGuard.cpp
    ${_src}/src/server/StaticFileHandler.cpp
    ${_src}/src/server/HttpParser.cpp
    ${_src}/src/common/Logger.cpp
)

target_include_directories(run_tests PRIVATE
    ${_src}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Limelight.h (header-only macros used by StreamConfig).
    ${_src}/third_party/moonlight-common-c/src
)

# Silence production qDebug() noise; keep qInfo()/qCritical() (test output).
target_compile_definitions(run_tests PRIVATE QT_NO_DEBUG_OUTPUT)

target_link_libraries(run_tests PRIVATE
    Qt6::Core Qt6::Network
    OpenSSL::SSL OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(run_tests PRIVATE ws2_32 winmm Bcrypt Crypt32)
    # Emit PDB debug info even in release so OpenCppCoverage can map line coverage.
    target_compile_options(run_tests PRIVATE /Zi)
    target_link_options(run_tests PRIVATE /DEBUG)
    # Ship the OpenSSL runtime DLLs next to the runner (needed at launch).
    file(GLOB _ssl_dlls "${_ssl}/lib/x64/*-3-x64.dll")
    foreach(_dll ${_ssl_dlls})
        add_custom_command(TARGET run_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:run_tests>")
    endforeach()
elseif(UNIX AND NOT APPLE)
    target_link_libraries(run_tests PRIVATE pthread dl)
endif()

enable_testing()
add_test(NAME backend-tnr COMMAND run_tests)
