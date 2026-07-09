# ============================================================================
# MoonlightWeb backend — cross-platform build (Windows x64/ARM64, Linux, macOS)
# Replaces the Windows-only qmake (.pro) + .bat toolchain.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# Native deps (libdatachannel, miniupnpc, qmdnsengine, moonlight-common-c) are
# built statically from their own upstream CMakeLists via git submodules.
# OpenSSL: system package on Linux/macOS, prebuilt under libs/windows on Windows.
# ============================================================================
cmake_minimum_required(VERSION 3.21)
project(moonlightweb-backend VERSION 0.1.1 LANGUAGES C CXX)

# Single source of truth for the shipped version string: defaults to the project
# version; the release workflow overrides it from the v* tag (-DMW_VERSION=x.y.z)
# so the binary, installers and packages all report the same version.
set(MW_VERSION "${PROJECT_VERSION}" CACHE STRING "Application version string")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Export compile_commands.json (clangd / clang-tidy / IDE IntelliSense).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All third-party deps are linked statically into mw-server.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── OpenSSL ────────────────────────────────────────────────────────────────
# Windows: prebuilt under libs/windows (committed). Non-standard layout
# (include/<arch>/openssl, lib/<arch>, import libs -> DLLs) so we point the
# OpenSSL_* cache vars explicitly; they propagate to the dep subprojects too.
# Linux/macOS: system package via find_package.
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_mw_ssl_arch arm64)
    else()
        set(_mw_ssl_arch x64)
    endif()
    set(_mw_ssl "${CMAKE_CURRENT_SOURCE_DIR}/libs/windows")
    # Use the committed prebuilt only if present for this arch (x64). Otherwise
    # (e.g. ARM64) fall back to find_package — supply OpenSSL via a vcpkg
    # toolchain or OPENSSL_ROOT_DIR on the command line.
    if(EXISTS "${_mw_ssl}/lib/${_mw_ssl_arch}/libssl.lib")
        set(_mw_ssl_prebuilt ON)
        set(OPENSSL_ROOT_DIR     "${_mw_ssl}")
        set(OPENSSL_INCLUDE_DIR  "${_mw_ssl}/include/${_mw_ssl_arch}" CACHE PATH "" FORCE)
        set(OPENSSL_SSL_LIBRARY    "${_mw_ssl}/lib/${_mw_ssl_arch}/libssl.lib"    CACHE FILEPATH "" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "${_mw_ssl}/lib/${_mw_ssl_arch}/libcrypto.lib" CACHE FILEPATH "" FORCE)
    else()
        message(STATUS "No committed OpenSSL for ${_mw_ssl_arch} — relying on find_package")
    endif()
elseif(APPLE)
    # Homebrew keeps OpenSSL keg-only; help find_package locate it.
    foreach(_brew_ssl /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS "${_brew_ssl}")
            set(OPENSSL_ROOT_DIR "${_brew_ssl}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenSSL 1.1 REQUIRED)

# ── Qt 6 ───────────────────────────────────────────────────────────────────
# EntryPoint provides WinMain->main for the windowless (WIN32_EXECUTABLE) build.
find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets Widgets)
find_package(Qt6 QUIET COMPONENTS EntryPoint)
set(CMAKE_AUTOMOC ON)

# ── Threads (pthread on Unix) ──────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Native dependencies (static, built from submodules) ────────────────────

# moonlight-common-c (+ bundled ENet, nanors FEC). Its CMakeLists picks the
# right ENet platform sources (win32.c vs unix.c) and links OpenSSL crypto.
add_subdirectory(third_party/moonlight-common-c)

# libdatachannel (WebRTC DataChannel + media). Match the previous .bat flags.
set(NO_WEBSOCKET ON  CACHE BOOL "" FORCE)
set(NO_MEDIA     OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES  ON  CACHE BOOL "" FORCE)
set(NO_TESTS     ON  CACHE BOOL "" FORCE)
# Prevent libsrtp (sub-dep) from building its test drivers (cipher_driver, etc.),
# which otherwise clutter the run-target list in IDEs like Qt Creator.
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS   OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS  OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libdatachannel)

# miniupnpc (UPnP-IGD port mapping). Static only.
set(UPNPC_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/miniupnp/miniupnpc miniupnpc-build)

# miniupnpc ships flat headers in include/, but our code includes them as
# <miniupnpc/*.h>; stage a miniupnpc/-prefixed copy in the build tree.
file(COPY third_party/miniupnp/miniupnpc/include/
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/miniupnpc_include/miniupnpc)

# qmdnsengine (mDNS discovery). The vendored CMakeLists hardcodes Qt5, so build
# it ourselves as a static Qt6 library instead of add_subdirectory.
set(QMDNS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qmdnsengine/qmdnsengine/src)
configure_file(
    ${QMDNS_DIR}/qmdnsengine_export.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/qmdnsengine_gen/qmdnsengine_export.h
)
file(GLOB QMDNS_SOURCES ${QMDNS_DIR}/src/*.cpp)
file(GLOB QMDNS_HEADERS ${QMDNS_DIR}/include/qmdnsengine/*.h)
add_library(qmdnsengine STATIC ${QMDNS_SOURCES} ${QMDNS_HEADERS})
set_target_properties(qmdnsengine PROPERTIES AUTOMOC ON)
target_include_directories(qmdnsengine PUBLIC
    ${QMDNS_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/qmdnsengine_gen
)
target_link_libraries(qmdnsengine PUBLIC Qt6::Network)

# ── mw-server target ───────────────────────────────────────────────────────
# Explicit source list (excludes unused EnetControlStream/
# InputCrypto/InputEncoder which are not part of the current build).
add_executable(mw-server
    src/main.cpp
    src/server/AuthManager.cpp
    src/server/AppSettings.cpp
    src/server/Provisioning.cpp
    src/server/ConnectionGuard.cpp
    src/server/HttpServer.cpp
    src/server/ControlChannel.cpp
    src/server/HttpParser.cpp
    src/server/CertManager.cpp
    src/server/StaticFileHandler.cpp
    src/server/RestRouter.cpp
    src/server/routes/AuthRoutes.cpp
    src/server/routes/HostRoutes.cpp
    src/server/routes/SystemRoutes.cpp
    src/common/Logger.cpp
    src/common/CrashHandler.cpp
    src/network/UPNPClient.cpp
    src/network/PdnsClient.cpp
    src/network/StunClient.cpp
    src/network/AcmeClient.cpp
    src/network/InternetAccessManager.cpp
    src/network/GeoIpService.cpp
    src/network/UpdateChecker.cpp
    src/TrayManager.cpp
    src/Autostart.cpp
    src/backend/NvHTTP.cpp
    src/backend/NvComputer.cpp
    src/backend/ComputerManager.cpp
    src/backend/IdentityManager.cpp
    src/backend/NvPairingManager.cpp
    src/backend/SunshineRestClient.cpp
    src/backend/SunshineInstaller.cpp
    src/streaming/RelayBase.h # QObject base with no .cpp; listed so AUTOMOC mocs it
    src/streaming/StreamConfig.cpp
    src/streaming/Session.cpp
    src/streaming/MoonlightShim.cpp
    src/streaming/ClipboardBridge.cpp
    src/streaming/StreamRelay.cpp
    src/streaming/DataChannelRelay.cpp
    src/streaming/FrameSender.cpp
    src/streaming/MediaTrackRelay.cpp
    src/streaming/SignalingServer.cpp
)

# Ship the binary as "MoonlightWeb(.exe)" regardless of the CMake target name.
set_target_properties(mw-server PROPERTIES OUTPUT_NAME MoonlightWeb)

# App Nap suppression during streaming (Objective-C++, macOS only).
if(APPLE)
    target_sources(mw-server PRIVATE src/common/MacActivity.mm)
endif()

# Windows release builds run windowless (no console): logs go to a file next to
# the exe (see main.cpp). Debug keeps the console for development. WIN32_EXECUTABLE
# switches to /SUBSYSTEM:WINDOWS, so Qt6::EntryPoint must provide WinMain->main.
if(WIN32)
    set_target_properties(mw-server PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
    if(TARGET Qt6::EntryPoint)
        target_link_libraries(mw-server PRIVATE Qt6::EntryPoint)
    endif()
    # Embed the app icon so Explorer / taskbar / shortcuts show the logo.
    enable_language(RC)
    set(MW_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/assets/favicon.ico")
    configure_file(app_icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" @ONLY)
    target_sources(mw-server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
endif()

target_include_directories(mw-server PRIVATE
    src
    third_party/qmdnsengine/qmdnsengine/src/include
    third_party/qmdnsengine
    third_party/moonlight-common-c/src
    third_party/moonlight-common-c/enet/include
    ${CMAKE_CURRENT_BINARY_DIR}/miniupnpc_include
)

target_compile_definitions(mw-server PRIVATE
    RTC_STATIC
    RTC_ENABLE_WEBSOCKET=0
    RTC_ENABLE_MEDIA=1
    MW_HAVE_MINIUPNPC
    MINIUPNP_STATICLIB
    MW_VERSION="${MW_VERSION}"
    PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../"
    FRONTEND_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../frontend/"
    CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cert/"
)

# Optional build-time cert embedding (CI/production). Reads env vars only;
# multi-line PEM is passed pre-escaped by the CI. Skipped when unset.
if(DEFINED ENV{MW_CERT_PEM} AND NOT "$ENV{MW_CERT_PEM}" STREQUAL "")
    target_compile_definitions(mw-server PRIVATE MW_CERT_PEM="$ENV{MW_CERT_PEM}")
    message(STATUS "MW_CERT_PEM embedded at build time")
endif()
if(DEFINED ENV{MW_CERT_KEY} AND NOT "$ENV{MW_CERT_KEY}" STREQUAL "")
    target_compile_definitions(mw-server PRIVATE MW_CERT_KEY="$ENV{MW_CERT_KEY}")
    message(STATUS "MW_CERT_KEY embedded at build time")
endif()

# Optional build-time embedding of Internet-Access config (CI/production). Lets
# the distributed binary carry its DNS/ACME config without shipping a .env file.
# Values come from CI secrets (env vars at configure time); skipped when unset.
# A runtime env var / local .env still wins — see applyEmbeddedEnvDefaults().
foreach(_mw_var MW_DOMAIN MW_PDNS_URL MW_PDNS_TOKEN MW_ZEROSSL_EAB_KID MW_ZEROSSL_EAB_HMAC)
    if(DEFINED ENV{${_mw_var}} AND NOT "$ENV{${_mw_var}}" STREQUAL "")
        target_compile_definitions(mw-server PRIVATE ${_mw_var}="$ENV{${_mw_var}}")
        message(STATUS "${_mw_var} embedded at build time")
    endif()
endforeach()

target_link_libraries(mw-server PRIVATE
    Qt6::Core Qt6::Network Qt6::WebSockets Qt6::Widgets
    moonlight-common-c
    LibDataChannel::LibDataChannel
    libminiupnpc-static
    qmdnsengine
    OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads
)

# ── Platform-specific link libs ────────────────────────────────────────────
if(WIN32)
    target_link_libraries(mw-server PRIVATE ws2_32 winmm Bcrypt Crypt32 iphlpapi Dbghelp)
    # OpenSSL is linked dynamically (import libs) — ship the DLLs next to the exe.
    file(GLOB _mw_ssl_dlls "${_mw_ssl}/lib/${_mw_ssl_arch}/*-3-${_mw_ssl_arch}.dll")
    foreach(_dll ${_mw_ssl_dlls})
        add_custom_command(TARGET mw-server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:mw-server>")
    endforeach()
elseif(APPLE)
    target_link_libraries(mw-server PRIVATE
        "-framework Security" "-framework CoreFoundation" "-framework Foundation")
else() # Linux / other Unix
    target_link_libraries(mw-server PRIVATE dl)
endif()

# ── Debug symbols in Release (for crash minidump symbolization) ─────────────
# The crash handler (CrashHandler.cpp) writes a minidump on an unhandled
# exception, but a Release build symbolizes it only if a matching .pdb exists.
# /Zi keeps optimizations while emitting debug info; /DEBUG writes a SEPARATE
# .pdb (not shipped next to the exe) and /OPT:REF,/OPT:ICF restore the size
# folding that /DEBUG would otherwise turn off. Keep the .pdb from CI builds to
# symbolize dumps collected from released binaries.
if(MSVC)
    target_compile_options(mw-server PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(mw-server PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

# ── AddressSanitizer (opt-in: -DMW_ASAN=ON) ────────────────────────────────
option(MW_ASAN "Enable AddressSanitizer" OFF)
if(MW_ASAN)
    message(STATUS "AddressSanitizer ENABLED")
    if(MSVC)
        target_compile_options(mw-server PRIVATE /fsanitize=address /Zi)
        # MSVC ASan turns on STL container-overflow annotations, but only mw-server
        # is instrumented — the vendored static libs (qmdnsengine, …) are built
        # without ASan, so their non-annotated std::string/vector clash at link
        # time (LNK2038 annotate_string/vector/optional). Disabling the container
        # annotations keeps every other ASan check (heap/stack/use-after-free)
        # while letting instrumented and non-instrumented TUs link together.
        target_compile_definitions(mw-server PRIVATE
            _DISABLE_STRING_ANNOTATION=1
            _DISABLE_VECTOR_ANNOTATION=1)
    else()
        target_compile_options(mw-server PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(mw-server PRIVATE -fsanitize=address)
    endif()
endif()

# ── Cross-platform security tests (ctest) ──────────────────────────────────
# Lightweight runner over the pure-logic security units (ConnectionGuard:
# per-IP anti-DDoS / abuse ban). Depends on Qt Core/Network only — no OpenSSL,
# no Windows APIs — so the SAME security logic is verified on Linux, macOS and
# Windows, enforcing parity across platforms. The full Windows-only TNR suite
# (OpenCppCoverage, OpenSSL-dependent units) still lives in tests/tests.pro.
option(MW_BUILD_TESTS "Build cross-platform unit tests" ON)
if(MW_BUILD_TESTS)
    enable_testing()
    add_executable(mw-security-tests
        tests/security_main.cpp
        tests/test_connection_guard.cpp
        src/server/ConnectionGuard.cpp
    )
    target_include_directories(mw-security-tests PRIVATE src tests)
    target_link_libraries(mw-security-tests PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME security-tnr COMMAND mw-security-tests)
endif()

# ── Install / packaging ────────────────────────────────────────────────────
# Lays out a relocatable bundle: the executable, the frontend served as static
# files (HttpServer falls back to <exedir>/frontend at runtime), and the OpenSSL
# DLLs on Windows. Qt runtime is added per-platform by the deploy tool
# (windeployqt / macdeployqt / linuxdeploy) in the release workflow.
install(TARGETS mw-server RUNTIME DESTINATION . BUNDLE DESTINATION .)
# Ship only the served assets: dev tooling (node_modules, tests, lint/TS configs,
# internal .claude notes) must never reach user machines — and trips codesign on
# macOS. Top-level dirs are excluded by REGEX so e.g. js/ subtrees stay intact.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../frontend" DESTINATION .
    REGEX "/frontend/(\\.claude|node_modules|test|scripts|build|coverage)$" EXCLUDE
    PATTERN ".prettierignore" EXCLUDE
    PATTERN ".prettierrc.json" EXCLUDE
    PATTERN ".tolgeerc.json" EXCLUDE
    PATTERN "eslint.config.js" EXCLUDE
    PATTERN "package.json" EXCLUDE
    PATTERN "package-lock.json" EXCLUDE
    PATTERN "prompt-frontend.md" EXCLUDE
    PATTERN "tsconfig.json" EXCLUDE
    PATTERN "vitest.config.js" EXCLUDE
)
if(WIN32 AND _mw_ssl_prebuilt)
    install(FILES ${_mw_ssl_dlls} DESTINATION .)
endif()

set(CPACK_PACKAGE_NAME "moonlightweb")
set(CPACK_PACKAGE_VENDOR "moonlightweb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Sunshine streaming client for the browser")
set(CPACK_PACKAGE_VERSION "${MW_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "moonlightweb")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop;TGZ")
else()
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "moonlightweb")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
include(CPack)
