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
        src/platform/windows/CrossGpuBridge.cpp
        src/capture/windows/DxgiDuplication.cpp
        src/capture/windows/WgcCapture.cpp
        src/capture/windows/CursorShape.cpp
        src/capture/windows/Win32Cursor.cpp
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
        src/encode/windows/VplFrameAllocator.cpp
        src/encode/windows/VplCapabilities.cpp
        src/encode/windows/VplEncoder.cpp
        src/audio/windows/WasapiLoopback.cpp
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
    # avrt is MMCSS: the capture thread registers as a "Games" task so the
    # scheduler favours it over the desktop's background work (and the audio
    # thread as "Pro Audio").
    # ole32 is COM: WASAPI's device enumerator is a COM object.
    # runtimeobject is the Windows Runtime's ABI entry points — RoInitialize,
    # RoGetActivationFactory, the HSTRING helpers — which is how the WGC
    # fallback reaches Windows.Graphics.Capture without linking a C++/WinRT
    # projection (see WgcCapture.h on why the projection is unwanted here).
    target_link_libraries(mw-native-host PRIVATE dxgi d3d11 d3dcompiler user32 gdi32 setupapi
                                                 winmm avrt ole32 runtimeobject)
elseif(APPLE)
    set(MW_NATIVE_PLATFORM "macos")
    # ScreenCaptureKit → VideoToolbox, with no conversion stage in between:
    # the compositor writes the encoder's NV12 itself (SckCapture.h). Input by
    # Quartz events. Objective-C++ throughout, compiled as C++ sources with
    # ARC turned on per file — CMake's CXX language already knows .mm.
    set(MW_NATIVE_MACOS_SOURCES
        src/platform/macos/MacProbe.mm
        src/platform/macos/MacSession.mm
        src/capture/macos/SckCapture.mm
        src/encode/macos/VtEncoder.mm
        src/input/macos/CgInput.mm)
    target_sources(mw-native-host PRIVATE ${MW_NATIVE_MACOS_SOURCES})
    set_source_files_properties(${MW_NATIVE_MACOS_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # ScreenCaptureKit needs macOS 12.3; everything else here is older. The
    # deployment target is the top-level build's business (the .pkg says 12.0
    # today), so the 12.3 APIs are what the probe's isOsSupported() guards.
    target_link_libraries(mw-native-host PRIVATE
        "-framework ScreenCaptureKit"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Metal"
        "-framework IOKit"
        "-framework IOSurface"
        "-framework Security")
    # PUBLIC so the tests can tell whether the backend exists.
    target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_MACOS=1)
else()
    set(MW_NATIVE_PLATFORM "none")

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The input side of the Linux backend: virtual gamepad, keyboard and
        # mouse, all through uinput. Unconditional, unlike the graphics below:
        # uinput needs no library at all — it is kernel ioctls — so it builds on
        # any Linux, -dev packages or not, and stays honest at -Wall -Wextra on
        # every Linux CI run. Reached since the Linux session landed (04/09/2026,
        # §19): LinuxSession constructs both and routes the controller events to
        # the gamepad, and LinuxProbe::probeVirtualGamepad asks /dev/uinput
        # itself. On a build without the graphics backend they are compiled and
        # never constructed (the stub session refuses first).
        #
        # X11Pointer is unconditional for the same reason: libX11 is opened with
        # dlopen at runtime, never linked, so no -dev package is needed to build
        # it and a host with no X at all still runs — it simply finds nothing.
        target_sources(mw-native-host PRIVATE
            src/input/linux/UinputGamepad.cpp
            src/input/linux/UinputInput.cpp
            src/input/linux/X11Pointer.cpp)
        # dlopen lives in libc since glibc 2.34 and our floor is 2.35, so this
        # is empty on every platform we ship — kept because it is what makes the
        # dependency explicit rather than lucky.
        target_link_libraries(mw-native-host PRIVATE ${CMAKE_DL_LIBS})

        # The Linux graphics backend — KMS capture, EGL conversion, VA-API —
        # needs libdrm, libva, EGL, GLESv2 and GBM (all MIT, on the allow-list
        # in LICENSE.md). Found → built and exercised by the tests on a machine
        # that has a display; not found → the module still builds, with the
        # honest stub, and says so loudly rather than failing the configure.
        # That keeps a runner without the dev packages green while the packaging
        # workflow catches up.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MW_LINUX_GFX libdrm libva libva-drm egl glesv2 gbm)
        endif()
        if(MW_LINUX_GFX_FOUND)
            set(MW_NATIVE_PLATFORM "linux")
            message(STATUS "mw-native-host: Linux graphics backend ON (libdrm ${MW_LINUX_GFX_libdrm_VERSION}, libva ${MW_LINUX_GFX_libva_VERSION})")
            # The real platform seam replaces the stub: probe from KMS + VA-API,
            # session KMS → EGL → VA-API.
            target_sources(mw-native-host PRIVATE
                src/platform/linux/LinuxProbe.cpp
                src/platform/linux/LinuxSession.cpp
                src/capture/linux/KmsCapture.cpp
                src/convert/linux/GlConvert.cpp
                src/encode/linux/VaapiEncoder.cpp)
            target_include_directories(mw-native-host SYSTEM PRIVATE ${MW_LINUX_GFX_INCLUDE_DIRS})
            target_link_libraries(mw-native-host PRIVATE ${MW_LINUX_GFX_LIBRARIES})
            # PUBLIC so the tests can tell whether the backend exists.
            target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_GFX=1)

            # The sound: a PipeWire capture stream on the default output's
            # monitor (libpipewire-0.3, MIT — libpulse is LGPL and off the
            # allow-list in LICENSE.md). Optional on the same terms as the
            # graphics: found → the session carries the host's audio; not
            # found → it streams silent and the configure says why.
            pkg_check_modules(MW_LINUX_AUDIO libpipewire-0.3)
            if(MW_LINUX_AUDIO_FOUND)
                message(STATUS "mw-native-host: Linux audio (PipeWire ${MW_LINUX_AUDIO_VERSION}) ON")
                target_sources(mw-native-host PRIVATE src/audio/linux/PipeWireCapture.cpp)
                target_include_directories(mw-native-host SYSTEM PRIVATE ${MW_LINUX_AUDIO_INCLUDE_DIRS})
                target_link_libraries(mw-native-host PRIVATE ${MW_LINUX_AUDIO_LIBRARIES})
                target_compile_options(mw-native-host PRIVATE ${MW_LINUX_AUDIO_CFLAGS_OTHER})
                # PUBLIC so the session test knows whether to expect audio.
                target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_AUDIO=1)
            else()
                message(WARNING "mw-native-host: Linux audio OFF — install libpipewire-0.3-dev to build it")
            endif()
        else()
            message(WARNING "mw-native-host: Linux graphics backend OFF — install libdrm-dev libva-dev libegl1-mesa-dev libgles2-mesa-dev libgbm-dev to build it")
        endif()
    endif()

    if(MW_NATIVE_PLATFORM STREQUAL "none")
        target_sources(mw-native-host PRIVATE src/platform/Unimplemented.cpp)
    endif()
endif()

message(STATUS "mw-native-host: platform backend = ${MW_NATIVE_PLATFORM}")

# ── Audio ───────────────────────────────────────────────────────────────────
# The encoder and the pacing sink are platform-neutral and compiled wherever
# there is a capture to feed them: Windows (WASAPI loopback, its own thread),
# macOS (ScreenCaptureKit's audio tap, through PacedOpusSink) and Linux when
# libpipewire is there (the default output's monitor, through the same sink).
# Without a source libopus is not built — nothing would call it.
#
# libopus (BSD-3-Clause, on the allow-list in LICENSE.md) is built from the
# pinned submodule as a static library: nothing installed, nothing shared, no
# programs, no tests.
if(WIN32 OR APPLE OR MW_LINUX_AUDIO_FOUND)
    target_sources(mw-native-host PRIVATE
        src/audio/OpusEncoder.cpp
        src/audio/PacedOpusSink.cpp)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
    set_target_properties(opus PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(mw-native-host PRIVATE Opus::opus)
    target_include_directories(mw-native-host SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include)
endif()

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
        tests/test_win32_cursor.cpp
        tests/test_evdev_keymap.cpp
        tests/test_pointer_clamp.cpp
        tests/test_linux_pipeline.cpp
        tests/test_linux_session.cpp
        tests/test_mac_keymap.cpp
        tests/test_mac_session.cpp
        tests/test_vpl_params.cpp
        tests/test_ds4_mapping.cpp
        tests/test_stage_stats.cpp
        tests/test_frame_cadence.cpp
        tests/test_cadence_align.cpp
        tests/test_restart_backoff.cpp
        tests/test_rate_control.cpp
        tests/test_reference_slots.cpp
        tests/test_audio_pacer.cpp
        tests/test_audio_interleave.cpp
        tests/test_cursor_blend.cpp
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
    # Where the module encodes audio, the session tests decode it back with the
    # same libopus to look at the signal — the only proof this side of a browser
    # that the tap captured sound and not a perfectly paced silence.
    if(TARGET opus)
        target_link_libraries(mw-native-tests PRIVATE Opus::opus)
        target_include_directories(mw-native-tests SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/include)
        target_compile_definitions(mw-native-tests PRIVATE MW_NATIVE_TESTS_HAVE_OPUS=1)
    endif()
    add_test(NAME native-host COMMAND mw-native-tests)

    # The boundary rule is itself a test, so a failure shows up in the ctest
    # report rather than only in build output.
    add_test(NAME native-host-boundary
             COMMAND ${CMAKE_COMMAND}
                     -DMW_NATIVE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boundary_check.cmake)
endif()
