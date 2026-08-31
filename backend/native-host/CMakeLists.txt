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
    )
    # DXGI enumerates the adapters and their outputs (the display→GPU
    # association everything else depends on) and drives Desktop Duplication;
    # d3d11 owns the textures frames live in; user32/gdi32 supply the exact
    # refresh rate and the primary-monitor flag.
    target_link_libraries(mw-native-host PRIVATE dxgi d3d11 user32 gdi32)
else()
    set(MW_NATIVE_PLATFORM "none")
    target_sources(mw-native-host PRIVATE src/platform/Unimplemented.cpp)
endif()

message(STATUS "mw-native-host: platform backend = ${MW_NATIVE_PLATFORM}")

target_include_directories(mw-native-host
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
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
    )
    set_target_properties(mw-native-tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(mw-native-tests PRIVATE mw-native-host)
    target_include_directories(mw-native-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    add_test(NAME native-host COMMAND mw-native-tests)

    # The boundary rule is itself a test, so a failure shows up in the ctest
    # report rather than only in build output.
    add_test(NAME native-host-boundary
             COMMAND ${CMAKE_COMMAND}
                     -DMW_NATIVE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boundary_check.cmake)
endif()
