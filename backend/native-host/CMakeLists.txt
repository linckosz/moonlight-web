# ============================================================================
# mw-native-host — MoonlightWeb's own capture & encoding engine.
#
# Built as a STATIC library that links NOTHING from the host application: no
# Qt, no moonlight-common-c, no GPL dependency of any kind. That separation is
# not stylistic — it is what keeps this module relicensable on its own (see
# LICENSE.md), and cmake/boundary_check.cmake enforces it at build time.
#
# Platform backends are selected here, and exactly one is compiled. When none
# exists for the target yet, platform/Unimplemented.cpp answers "not available"
# truthfully, so the whole chain above stays exercisable.
# ============================================================================

set(MW_NATIVE_VERSION "0.1.0-dev")

add_library(mw-native-host STATIC
    src/core/Log.cpp
    src/core/Capabilities.cpp
    src/core/Probe.cpp
    src/core/Selector.cpp
    src/core/NativeHost.cpp
    # The per-stage latency histograms every native figure is read through.
    # Pure arithmetic, no platform, so it is testable — and tested — anywhere.
    src/core/StageStats.cpp
    # Platform-neutral on purpose, and compiled everywhere for that reason: the
    # DualShock 4 report is pure arithmetic, and it is the part of the profile
    # that can be wrong in an interesting way (an inverted axis, a D-pad
    # diagonal). Building it on every platform is what lets the tests check it
    # without a driver or a pad.
    src/input/Ds4Mapping.cpp
)

# ── Platform backend selection ──────────────────────────────────────────────
# Exactly one backend is compiled. Where a platform has none yet it uses the
# honest stub, which answers "not available here" — never a fake "available".
if(WIN32)
    set(MW_NATIVE_PLATFORM "windows")
    target_sources(mw-native-host PRIVATE
        src/platform/windows/WindowsProbe.cpp
        src/platform/windows/WindowsEncoderProbe.cpp
        src/platform/windows/WindowsSession.cpp
        src/capture/windows/DxgiDuplication.cpp
        src/convert/windows/ColorConvert.cpp
        src/input/windows/Win32Input.cpp
        src/input/windows/VigemGamepad.cpp
        third_party/vigem-client/src/ViGEmClient.cpp
        src/encode/windows/NvencApi.cpp
        src/encode/windows/NvencCapabilities.cpp
        src/encode/windows/NvencEncoder.cpp
        src/encode/windows/AmfApi.cpp
        src/encode/windows/AmfCapabilities.cpp
        src/encode/windows/AmfEncoder.cpp
        src/encode/windows/VplApi.cpp
        src/encode/windows/VplSession.cpp
        src/encode/windows/VplCapabilities.cpp
        src/encode/windows/VplEncoder.cpp
    )
    # DXGI enumerates the adapters and their outputs (the display→GPU
    # association everything else depends on) and drives Desktop Duplication;
    # d3d11 owns the textures frames live in; user32/gdi32 supply the exact
    # refresh rate and the primary-monitor flag.
    # d3dcompiler builds the colour-conversion shaders at session start (~1 ms,
    # once) — precompiling them would add fxc to the build for no runtime gain.
    # setupapi is ViGEmClient's: it finds the virtual bus by device interface.
    # winmm is timeBeginPeriod: the capture loop waits for the stream's next
    # due time, and without a 1 ms scheduler period that wait lands 15 ms late.
    target_link_libraries(mw-native-host PRIVATE dxgi d3d11 d3dcompiler user32 gdi32 setupapi
                                                 winmm)
else()
    set(MW_NATIVE_PLATFORM "none")
    target_sources(mw-native-host PRIVATE src/platform/Unimplemented.cpp)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The virtual gamepad, ahead of the rest of the Linux backend.
        #
        # Compiled but NOT yet reachable: there is no Linux session to route
        # input from (no capture, no encoder), so nothing constructs it and
        # probeVirtualGamepad() still answers "no backend on this platform" —
        # which is the truth a user can act on today. Building it here is what
        # keeps it honest: it compiles at -Wall -Wextra on every Linux CI run,
        # so it cannot rot into the sort of "ready" code that has never once
        # been through a compiler.
        target_sources(mw-native-host PRIVATE src/input/linux/UinputGamepad.cpp)
    endif()
endif()

message(STATUS "mw-native-host: platform backend = ${MW_NATIVE_PLATFORM}")

target_include_directories(mw-native-host
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The three vendor SDKs, as SYSTEM includes.
#
# SYSTEM because their warnings are not ours to fix: oneVPL's headers use
# nameless unions, which /W4 objects to, and this module compiles at /W4 with a
# deliberately clean slate. Silencing them per-warning would hide the same
# warning in OUR code; scoping the exemption to the vendor trees does not.
#
# Only headers, for all three: every runtime — nvEncodeAPI64.dll, amfrt64.dll,
# libvpl.dll — ships with its vendor's driver and is resolved at run time, so
# nothing here is linked and nothing of any SDK is redistributed.
target_include_directories(mw-native-host SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc-headers
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf-headers
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vpl-headers
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vigem-client/include
)

target_compile_features(mw-native-host PUBLIC cxx_std_17)

# The top-level build turns CMAKE_AUTOMOC on globally. This module has no
# Q_OBJECT and never will, so running moc over it is pure build time — and
# leaving Qt's tooling attached to a target that must stay Qt-free invites the
# very dependency the boundary check exists to prevent.
set_target_properties(mw-native-host PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

target_compile_definitions(mw-native-host PRIVATE MW_NATIVE_VERSION="${MW_NATIVE_VERSION}")

# Deliberately NOT linked: Qt6::*, moonlight-common-c, anything GPL. Threads is
# the only dependency this module has today, and every future one must come
# from the allow-list in LICENSE.md.
find_package(Threads REQUIRED)
target_link_libraries(mw-native-host PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(mw-native-host PRIVATE /W4)
else()
    target_compile_options(mw-native-host PRIVATE -Wall -Wextra)
endif()

# ── Licence boundary guardrail ──────────────────────────────────────────────
# Runs on every build of the library, not just when someone remembers to run
# the tests: the whole point is that a forbidden #include cannot reach a commit.
add_custom_target(mw-native-host-boundary ALL
    COMMAND ${CMAKE_COMMAND}
            -DMW_NATIVE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boundary_check.cmake
    COMMENT "Checking mw-native-host licence boundary"
    VERBATIM
)
add_dependencies(mw-native-host mw-native-host-boundary)

# ── Tests ───────────────────────────────────────────────────────────────────
# Qt-free on purpose, like the module: the policy that decides GPU, encoder and
# codec is pure logic and must be verifiable on a machine with no GPU at all —
# including CI.
if(MW_BUILD_TESTS)
    add_executable(mw-native-tests
        tests/native_main.cpp
        tests/test_selector.cpp
        tests/test_capabilities.cpp
        tests/test_capture.cpp
        tests/test_vpl_params.cpp
        tests/test_ds4_mapping.cpp
        tests/test_stage_stats.cpp
        tests/test_frame_cadence.cpp
        tests/test_restart_backoff.cpp
        tests/test_rate_control.cpp
    )
    set_target_properties(mw-native-tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(mw-native-tests PRIVATE mw-native-host)
    target_include_directories(mw-native-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    # The tests drive the platform classes directly — that is the point, it is
    # where the real hardware gets exercised — so they need the same vendor
    # headers the implementation does, on the same terms.
    target_include_directories(mw-native-tests SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf-headers
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vpl-headers
    )
    add_test(NAME native-host COMMAND mw-native-tests)

    # The boundary rule is itself a test, so a failure shows up in the ctest
    # report rather than only in build output.
    add_test(NAME native-host-boundary
             COMMAND ${CMAKE_COMMAND}
                     -DMW_NATIVE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boundary_check.cmake)
endif()
