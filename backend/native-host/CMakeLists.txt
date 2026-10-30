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
    # The HID passthrough's descriptor, rebuilt from WebHID's collections and
    # checked before the host creates anything. Pure as well, so tested anywhere.
    src/input/HidDescriptor.cpp
    # The host's own mouse on a Windows host with no pointing device: its one
    # fixed descriptor and its reports. Pure, tested anywhere.
    src/input/SystemMouse.cpp
    src/input/HidPid.cpp
    # A wheel's force feedback as the game left it, replayed to the next page.
    src/input/HidFfbJournal.cpp
    # A wheel's evdev layout and Linux force feedback effects (uinput, P4).
    src/input/HidEvdev.cpp
    # The devices a session recreates from them, with their watchdog, and the
    # parking that keeps them between two streams; the OS backend behind them
    # is chosen below (uhid, the driver, or none).
    src/input/HidPassthrough.cpp
)

# ── Platform backend selection ──────────────────────────────────────────────
# Exactly one backend is compiled. Where a platform has none yet it uses the
# honest stub, which answers "not available here" — never a fake "available".
if(WIN32)
    set(MW_NATIVE_PLATFORM "windows")
    target_sources(mw-native-host PRIVATE
        src/platform/windows/WindowsProbe.cpp
        src/platform/windows/WindowsEncoderProbe.cpp
        src/platform/windows/IndirectDisplay.cpp
        src/platform/windows/DisplayModeHold.cpp
        src/platform/windows/DisplayOutputs.cpp
        src/platform/windows/WindowsSession.cpp
        src/platform/windows/InputDesktop.cpp
        src/platform/windows/CrossGpuBridge.cpp
        src/platform/windows/StreamPriority.cpp
        src/platform/windows/Win32PointerSystem.cpp
        src/platform/windows/video/D3d11VideoPipeline.cpp
        src/platform/windows/video/D3d12VideoPipeline.cpp
        src/platform/windows/d3d12/D3d12Device.cpp
        src/platform/windows/d3d12/DdaInterop.cpp
        src/capture/windows/DxgiDuplication.cpp
        src/capture/windows/WgcCapture.cpp
        src/capture/windows/CursorShape.cpp
        src/capture/windows/Win32Cursor.cpp
        src/convert/windows/ColorConvert.cpp
        src/convert/windows/ConvertShaders.cpp
        src/convert/windows/MwsrStats.cpp
        src/convert/windows/PassTimer.cpp
        src/convert/windows/d3d12/ColorConvert12.cpp
        src/input/windows/Win32Input.cpp
        src/input/windows/WinTextKeys.cpp
        src/input/windows/VigemGamepad.cpp
        third_party/vigem-client/src/ViGEmClient.cpp
        src/encode/windows/NvencApi.cpp
        src/encode/windows/NvencCapabilities.cpp
        src/encode/windows/NvencConfig.cpp
        src/encode/windows/NvencEncoder.cpp
        src/encode/windows/AmfApi.cpp
        src/encode/windows/AmfCapabilities.cpp
        src/encode/windows/AmfConfig.cpp
        src/encode/windows/AmfEncoder.cpp
        src/encode/windows/VplApi.cpp
        src/encode/windows/VplSession.cpp
        src/encode/windows/VplFrameAllocator.cpp
        src/encode/windows/VplCapabilities.cpp
        src/encode/windows/VplEncoder.cpp
        src/encode/windows/MfApi.cpp
        src/encode/windows/MfCapabilities.cpp
        src/encode/windows/MfEncoder.cpp
        src/encode/windows/SoftwareEncoder.cpp
        src/encode/windows/d3d12/VideoEncodeCaps12.cpp
        src/encode/windows/d3d12/VideoEncode12.cpp
        src/encode/windows/d3d12/NvencEncoder12.cpp
        src/encode/windows/d3d12/AmfEncoder12.cpp
        src/encode/windows/d3d12/PyroWaveEncoder12.cpp
        src/encode/windows/d3d12/UltraEncoder12.cpp
        src/audio/windows/WasapiLoopback.cpp
        src/audio/windows/HostMute.cpp
    )

    # DXGI enumerates the adapters and their outputs (the display→GPU
    # association everything else depends on) and drives Desktop Duplication;
    # d3d11 owns the textures frames live in; user32/gdi32 supply the exact
    # refresh rate and the primary-monitor flag.
    # d3dcompiler builds the colour-conversion shaders at session start (~1 ms,
    # once) — precompiling them would add fxc to the build for no runtime gain.
    # setupapi is ViGEmClient's: it finds the virtual bus by device interface.
    # avrt is MMCSS: the capture thread registers as a "Games" task so the
    # scheduler favours it over the desktop's background work (and the audio
    # thread as "Pro Audio").
    # d3d12 is the cross-GPU copy's readback — its COPY queue runs on the
    # GPU's DMA engine, out of the way of a game holding the 3D engine
    # (CrossGpuBridge) — and the D3D12 video pipeline's device, queues and
    # fences (platform/windows/d3d12).
    # ole32 is COM: WASAPI's device enumerator is a COM object.
    # runtimeobject is the Windows Runtime's ABI entry points — RoInitialize,
    # RoGetActivationFactory, the HSTRING helpers — which is how the WGC
    # fallback reaches Windows.Graphics.Capture without linking a C++/WinRT
    # projection (see WgcCapture.h on why the projection is unwanted here).
    # mfuuid and strmiids are the only Media Foundation libraries linked, and
    # they are safe to link precisely because they have no DLL behind them: both
    # are static tables of GUID constants (the media types, and the ICodecAPI
    # properties). `mfplat.dll` itself is loaded at run time (MfApi.h) so that
    # an N edition of Windows — where it is simply absent — still starts.
    # POC Ultra: PyroWaveEncoder12's HLSL, embedded as a string at configure
    # time and compiled at run time by d3dcompiler (shader model 5.0), as the
    # conversion's shaders are.
    set(_mw_pyrowave_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/src/encode/windows/d3d12/PyroWaveEncoder12.hlsl)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_mw_pyrowave_hlsl})
    file(READ ${_mw_pyrowave_hlsl} _mw_pyrowave_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _mw_pyrowave_bytes "${_mw_pyrowave_hex}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/PyroWaveEncoder12Hlsl.h
         "// Generated from PyroWaveEncoder12.hlsl at configure time.\n#pragma once\n"
         "static const char kPyroWaveEncoderHlsl[] = {${_mw_pyrowave_bytes}0x00};\n")
    target_include_directories(mw-native-host PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

    # dwmapi: the compositor's timing, read by the bench's click trace only.
    target_link_libraries(mw-native-host PRIVATE dxgi d3d11 d3d12 d3dcompiler user32 gdi32 setupapi
                                                 avrt ole32 oleaut32 runtimeobject mfuuid
                                                 strmiids dwmapi wtsapi32 imm32)
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
        src/input/macos/CgInput.mm
        src/input/macos/MacPointerAccel.mm)
    target_sources(mw-native-host PRIVATE ${MW_NATIVE_MACOS_SOURCES})
    # Plain C++ against the CoreAudio C API — no Objective-C, hence outside the
    # ARC list above.
    target_sources(mw-native-host PRIVATE src/audio/macos/HostMute.cpp)
    # The virtual display drives PRIVATE CoreGraphics classes through
    # objc_msgSend casts and key-value coding, and keeps the one object that
    # matters alive with an explicit retain. Deliberately outside the ARC list:
    # ARC cannot see through a cast call, and would either leak or over-release
    # what comes back from `initWithDescriptor:`.
    target_sources(mw-native-host PRIVATE src/platform/macos/MacVirtualDisplay.mm)
    set_source_files_properties(${MW_NATIVE_MACOS_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # ScreenCaptureKit needs macOS 12.3; everything else here is older. The
    # deployment target is the top-level build's business (the .pkg says 12.0
    # today), so the 12.3 APIs are what the probe's isOsSupported() guards.
    target_link_libraries(mw-native-host PRIVATE
        "-framework ScreenCaptureKit"
        "-framework VideoToolbox"
        "-framework CoreAudio"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Metal"
        "-framework IOKit"
        "-framework IOSurface"
        # Carbon, for the Text Input Sources API only: CgInput.mm asks the
        # current keyboard layout what character a virtual key produces
        # (TISCopyCurrentKeyboardLayoutInputSource, kTISPropertyUnicodeKeyLayoutData
        # and UCKeyTranslate), which is the sole way to type into a host whose
        # layout is not the viewer's. It used to link without being named, by
        # way of ApplicationServices re-exporting HIToolbox; recent SDKs dropped
        # that re-export and the link then fails on those symbols alone.
        "-framework Carbon"
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
        # X11Pointer, X11Layout, WaylandLayout, XkbTextMap and EiInput are
        # unconditional for the same reason: libX11, libXrandr,
        # libwayland-client, libxkbcommon and libei are opened with dlopen at
        # runtime, never linked, so no -dev package is needed to build them and
        # a host with none of them still runs — it simply finds nothing.
        target_sources(mw-native-host PRIVATE
            src/input/linux/EiInput.cpp
            src/input/linux/UinputGamepad.cpp
            src/input/linux/UinputInput.cpp
            src/input/linux/WaylandLayout.cpp
            src/input/linux/X11Layout.cpp
            src/input/linux/X11Pointer.cpp
            src/input/linux/XkbTextMap.cpp)
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
                src/platform/linux/SleepInhibit.cpp
                src/capture/linux/KmsCapture.cpp
                src/capture/linux/X11Damage.cpp
                src/convert/linux/GlConvert.cpp
                src/convert/linux/CpuConvert.cpp
                src/convert/linux/GlReadback.cpp
                src/encode/linux/VaapiEncoder.cpp
                src/platform/linux/VaapiLibrary.cpp)
            target_include_directories(mw-native-host SYSTEM PRIVATE ${MW_LINUX_GFX_INCLUDE_DIRS})
            # libva and libva-drm are compiled against but NOT linked: they are
            # opened on first use (src/platform/linux/LazyLibrary.h), so a
            # machine without them still starts the program — it only loses the
            # hardware encoder. Linked, they were NEEDED entries that stopped the
            # whole binary before main(). release.yml asserts they stay out.
            set(_mw_gfx_link ${MW_LINUX_GFX_LIBRARIES})
            list(FILTER _mw_gfx_link EXCLUDE REGEX "^va(-drm)?$")
            target_link_libraries(mw-native-host PRIVATE ${_mw_gfx_link})
            # PUBLIC so the tests can tell whether the backend exists.
            target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_GFX=1)

            # The split route's conversion (plan pipeline-video-d3d12-v2, §9-17):
            # Vulkan on a compute queue in front of VA-API, beside a game rather
            # than behind it. Nothing links libvulkan — the loader is opened with
            # dlopen, and a host without Vulkan converts through GL as before; the
            # headers are the module's own (third_party/vulkan-headers, Apache-2.0
            # OR MIT, in LICENSE.md). The shaders are GLSL built to SPIR-V by
            # glslangValidator (glslang-tools): a build tool, nothing of it ships.
            # Without it the route is left out, loudly, and GL converts.
            find_program(MW_GLSLANG_VALIDATOR glslangValidator)
            if(MW_GLSLANG_VALIDATOR)
                set(_mw_spirv ${CMAKE_CURRENT_BINARY_DIR}/spirv)
                set(_mw_shaders ${CMAKE_CURRENT_SOURCE_DIR}/src/convert/linux/shaders)
                set(_mw_spirv_headers "")
                foreach(_mw_shader "VkScaleHSpv;vk_scale.comp;-DMW_HORIZONTAL=1"
                                   "VkScaleVSpv;vk_scale.comp;-DMW_HORIZONTAL=0"
                                   "VkNv12Spv;vk_nv12.comp;")
                    list(GET _mw_shader 0 _mw_name)
                    list(GET _mw_shader 1 _mw_source)
                    list(LENGTH _mw_shader _mw_parts)
                    set(_mw_define "")
                    if(_mw_parts GREATER 2)
                        list(GET _mw_shader 2 _mw_define)
                    endif()
                    add_custom_command(
                        OUTPUT ${_mw_spirv}/${_mw_name}.h
                        COMMAND ${CMAKE_COMMAND} -E make_directory ${_mw_spirv}
                        COMMAND ${MW_GLSLANG_VALIDATOR} -V --target-env vulkan1.1 ${_mw_define}
                                --vn k${_mw_name} -o ${_mw_spirv}/${_mw_name}.h
                                ${_mw_shaders}/${_mw_source}
                        DEPENDS ${_mw_shaders}/${_mw_source}
                        VERBATIM)
                    list(APPEND _mw_spirv_headers ${_mw_spirv}/${_mw_name}.h)
                endforeach()
                target_sources(mw-native-host PRIVATE
                    src/platform/linux/vulkan/VulkanDevice.cpp
                    src/convert/linux/VulkanConvert.cpp
                    src/encode/linux/VulkanHevcEncoder.cpp
                    src/encode/linux/VulkanHevcDecoder.cpp
                    src/encode/linux/VulkanHevcProof.cpp
                    src/encode/linux/VulkanAv1Encoder.cpp
                    src/encode/linux/Dav1dDecoder.cpp
                    ${_mw_spirv_headers})
                # The proof's cached verdicts carry the app's version, so each
                # update proves again. MW_VERSION comes from backend/; given to
                # this one file, a new version recompiles only it.
                set_property(SOURCE src/encode/linux/VulkanHevcProof.cpp APPEND PROPERTY
                    COMPILE_DEFINITIONS MW_APP_VERSION="${MW_VERSION}")
                target_include_directories(mw-native-host PRIVATE ${_mw_spirv})
                target_include_directories(mw-native-host SYSTEM PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include)
                # The AV1 encoder's pixel proof reads its stream back with
                # dav1d (C13.12; BSD-2-Clause): headers only, from the system
                # (libdav1d-dev), the library opened at run time like libva.
                # Without them AV1 is never offered through Vulkan, and the
                # configure says so.
                pkg_check_modules(MW_DAV1D dav1d)
                if(MW_DAV1D_FOUND)
                    target_include_directories(mw-native-host SYSTEM PRIVATE ${MW_DAV1D_INCLUDE_DIRS})
                    target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_DAV1D=1)
                    message(STATUS "mw-native-host: AV1 pixel proof ON (dav1d ${MW_DAV1D_VERSION} headers)")
                else()
                    message(WARNING "mw-native-host: AV1 pixel proof OFF — install libdav1d-dev (no AV1 through Vulkan Video meanwhile)")
                endif()
                # PUBLIC so the session and the tests know whether the route exists.
                target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_VULKAN=1)
                message(STATUS "mw-native-host: Vulkan conversion ON (SPIR-V by ${MW_GLSLANG_VALIDATOR})")
            else()
                message(WARNING "mw-native-host: Vulkan conversion OFF — install glslang-tools to build it (the split route, GL converts meanwhile)")
            endif()

            # The sound: a PipeWire capture stream on the default output's
            # monitor (libpipewire-0.3, MIT — libpulse is LGPL and off the
            # allow-list in LICENSE.md). Optional on the same terms as the
            # graphics: found → the session carries the host's audio; not
            # found → it streams silent and the configure says why.
            pkg_check_modules(MW_LINUX_AUDIO libpipewire-0.3)
            if(MW_LINUX_AUDIO_FOUND)
                message(STATUS "mw-native-host: Linux audio (PipeWire ${MW_LINUX_AUDIO_VERSION}) ON")
                # Silencing the speakers is the same library and the same
                # graph, so it lives or dies with the tap (src/audio/linux/
                # HostMute.h has the measurement that shaped it).
                target_sources(mw-native-host PRIVATE src/audio/linux/PipeWireCapture.cpp
                                                      src/audio/linux/HostMute.cpp
                                                      src/audio/linux/PipeWireLibrary.cpp)
                target_include_directories(mw-native-host SYSTEM PRIVATE ${MW_LINUX_AUDIO_INCLUDE_DIRS})
                # Headers only, like libva above: libpipewire is opened on first
                # use (PipeWireLibrary.cpp), never linked. Linked, it was the
                # NEEDED entry the AppImage catalog's test machine did not have
                # (29/09/2026), and the program would not start at all.
                target_compile_options(mw-native-host PRIVATE ${MW_LINUX_AUDIO_CFLAGS_OTHER})
                # PUBLIC so the session test knows whether to expect audio.
                target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_AUDIO=1)

                # The ScreenCast portal — the only capture route an AppImage
                # can have, since it carries no capability (§19.8). It needs
                # both PipeWire (already found, for the stream) and D-Bus (for
                # the handshake), which is why it sits inside the audio block.
                #
                # ⚠️ sd-bus is LGPL-2.1+, the ONE exception to this module's
                # permissive-only rule, decided 08/09/2026 and bounded to this
                # path. LICENSE.md § "L'exception sd-bus" has the reasoning and
                # what it costs; do not widen it without re-reading that.
                pkg_check_modules(MW_LINUX_PORTAL libsystemd)
                if(MW_LINUX_PORTAL_FOUND)
                    message(STATUS "mw-native-host: ScreenCast portal ON (sd-bus ${MW_LINUX_PORTAL_VERSION}, LGPL — see LICENSE.md)")
                    # MutterDisplayConfig: GNOME's layout around the portal's
                    # virtual monitor — the same library, on the same route.
                    # MutterScreenCast: GNOME's own screen cast, the virtual
                    # display with no dialog (plan Idées Punktfunk C2), and
                    # SharedMonitor the monitor its streams share (no sd-bus).
                    # KwinVirtualOutput: KDE's virtual monitor, which its portal
                    # does not make (libwayland-client by dlopen, no sd-bus).
                    target_sources(mw-native-host PRIVATE src/capture/linux/PortalScreenCast.cpp
                                                          src/capture/linux/PortalCapture.cpp
                                                          src/capture/linux/MutterDisplayConfig.cpp
                                                          src/capture/linux/MutterScreenCast.cpp
                                                          src/capture/linux/SharedMonitor.cpp
                                                          src/capture/linux/KwinVirtualOutput.cpp
                                                          src/capture/linux/GamescopeSession.cpp
                                                          src/capture/linux/XFixesCursor.cpp)
                    target_include_directories(mw-native-host SYSTEM PRIVATE ${MW_LINUX_PORTAL_INCLUDE_DIRS})
                    target_link_libraries(mw-native-host PRIVATE ${MW_LINUX_PORTAL_LIBRARIES})
                    # PUBLIC so the tests know whether the route is built.
                    target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_LINUX_PORTAL=1)
                else()
                    message(WARNING "mw-native-host: ScreenCast portal OFF — install libsystemd-dev to build it")
                endif()
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

# The in-process virtual display (include/mw/native/VirtualDisplay.h) exists
# on macOS only so far. Everywhere else the honest stub answers "not here":
# on Windows by design (the display is a driver, handled above this module),
# on Linux until the portal's VIRTUAL source lands.
if(NOT APPLE)
    target_sources(mw-native-host PRIVATE src/platform/VirtualDisplayUnsupported.cpp)
endif()

# ── The CPU encoder ─────────────────────────────────────────────────────────
# OpenH264, the last rung of the fallback tier on Windows and the only encoder
# a Linux machine without a render node has. Built from the pinned submodule
# by third_party/openh264.cmake (encoder only, SIMD kernels when an assembler
# is there). Not built for macOS: VideoToolbox is the OS's encoder and has its
# own software fall-back inside the same API (MacProbe.mm says why).
if(WIN32 OR (UNIX AND NOT APPLE))
    include(third_party/openh264.cmake)
    target_sources(mw-native-host PRIVATE src/encode/OpenH264Encoder.cpp)
    target_link_libraries(mw-native-host PRIVATE mw-openh264)
    # PUBLIC so the tests know whether to expect the encoder.
    target_compile_definitions(mw-native-host PUBLIC MW_NATIVE_OPENH264=1)
endif()

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

# The HID passthrough's virtual devices (plan P2): uhid on Linux, the
# "MoonlightWeb Virtual HID" driver on Windows through WinUHid's client library
# (third_party/winuhid, MIT; the driver itself builds apart, drivers/vhid),
# nothing on macOS.
if(WIN32)
    target_sources(mw-native-host PRIVATE
        src/input/windows/VhidDevice.cpp
        third_party/winuhid/WinUHid/WinUHid.cpp)
    target_include_directories(mw-native-host SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/winuhid/WinUHid)
    target_compile_definitions(mw-native-host PRIVATE WINUHID_STATIC MOONLIGHTWEB_VHID)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(mw-native-host PRIVATE src/input/linux/UhidDevice.cpp
                                          src/input/linux/UinputWheel.cpp)
else()
    target_sources(mw-native-host PRIVATE src/input/VirtualHid_none.cpp)
endif()

# This machine's keyboard layout (include/mw/native/HostKeyboardLayout.h), for
# a Sunshine or a Wolf on the same machine that types text by pressing keys:
# read through XkbTextMap on Linux, and needed nowhere else.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(mw-native-host PRIVATE src/input/linux/HostKeyboardLayout.cpp)
else()
    target_sources(mw-native-host PRIVATE src/input/HostKeyboardLayout_none.cpp)
endif()

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
        tests/test_color_convert.cpp
        tests/test_color_convert12.cpp
        tests/test_d3d11_video_pipeline.cpp
        tests/test_d3d12_device.cpp
        tests/test_dda_interop.cpp
        tests/test_video_encode_caps12.cpp
        tests/test_video_encode12.cpp
        tests/test_encoder_configs.cpp
        tests/test_vendor_encode12.cpp
        tests/test_win32_cursor.cpp
        tests/test_win_text_keys.cpp
        tests/test_host_mute.cpp
        tests/test_evdev_keymap.cpp
        tests/test_pointer_clamp.cpp
        tests/test_recentre_detector.cpp
        tests/test_wheel_carry.cpp
        tests/test_xkb_text_map.cpp
        tests/test_absolute_map.cpp
        tests/test_wayland_layout.cpp
        tests/test_monitor_layout.cpp
        tests/test_gamescope.cpp
        tests/test_x11_layout.cpp
        tests/test_scanout_window.cpp
        tests/test_linux_pipeline.cpp
        tests/test_vulkan_convert.cpp
        tests/test_vulkan_hevc.cpp
        tests/test_vulkan_av1.cpp
        tests/test_linux_session.cpp
        tests/test_mac_keymap.cpp
        tests/test_mac_session.cpp
        tests/test_vpl_params.cpp
        tests/test_openh264.cpp
        tests/test_bgra_to_i420.cpp
        tests/test_ds4_mapping.cpp
        tests/test_hid_descriptor.cpp
        tests/test_system_mouse.cpp
        tests/test_hid_passthrough.cpp
        tests/test_hid_pid.cpp
        tests/test_hid_evdev.cpp
        tests/test_stage_stats.cpp
        tests/test_frame_cadence.cpp
        tests/test_cadence_lock.cpp
        tests/test_input_admit.cpp
        tests/test_capture_hold.cpp
        tests/test_resample_cost.cpp
        tests/test_mwsr_zones.cpp
        tests/test_mwsr_stats.cpp
        tests/test_cadence_align.cpp
        tests/test_cadence_choice.cpp
        tests/test_cadence_step.cpp
        tests/test_decode_credit.cpp
        tests/test_deadline_cadence.cpp
        tests/test_click_trace.cpp
        tests/test_restart_backoff.cpp
        tests/test_rate_control.cpp
        tests/test_qp_rate_controller.cpp
        tests/test_virtual_display.cpp
        tests/test_encode_load_cap.cpp
        tests/test_h264_vui.cpp
        tests/test_parameter_sets.cpp
        tests/test_hevc_slice_parser.cpp
        tests/test_hevc_dpb.cpp
        tests/test_hevc_negotiation.cpp
        tests/test_h264_slice_parser.cpp
        tests/test_h264_negotiation.cpp
        tests/test_av1_obu.cpp
        tests/test_video_pipeline_choice.cpp
        tests/test_d3d12_fault.cpp
        tests/test_linux_route_choice.cpp
        tests/test_reference_slots.cpp
        tests/test_intra_refresh_sweep.cpp
        tests/test_audio_pacer.cpp
        tests/test_audio_interleave.cpp
        tests/test_cursor_blend.cpp
        tests/test_frame_fit.cpp
        tests/test_painted_pointer.cpp
        tests/test_output_match.cpp
        tests/test_display_mode_hold.cpp
        tests/test_cursor_position_gate.cpp
        tests/test_pointer_presence.cpp
        tests/test_feed_header.cpp
        tests/test_feed_arbiter.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include
    )
    # Same terms for libdrm, which lives outside the default include path: the
    # Linux pipeline test asks the kernel about the display's planes itself, so
    # that what the capture reports can be checked against a second opinion.
    if(MW_LINUX_GFX_FOUND)
        target_include_directories(mw-native-tests SYSTEM PRIVATE ${MW_LINUX_GFX_INCLUDE_DIRS})
    endif()
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

# ── Lab tools ───────────────────────────────────────────────────────────────
# Instruments for deciding things about the engine, never shipped: off by
# default, outside the library's licence boundary (tools/ is not scanned; the
# D3D12 lab links mw-native-host to time the engine's own classes, never the
# other way round). On Windows a D3D11 shader bench and the D3D12 lab, on macOS
# the probe that measures the pointer acceleration MacPointerAccel has to
# reproduce, on Linux the Vulkan lab, and everywhere a game-like GPU load.
option(MW_BUILD_TOOLS "Build the native host's lab tools (scaler bench, mouse probe)" OFF)
if(MW_BUILD_TOOLS AND WIN32)
    add_subdirectory(tools/scaler-bench)
    add_subdirectory(tools/d3d12-lab)
    # Plan « attente »: the ideal game, the click drawn and presented at once.
    add_subdirectory(tools/click-target)
endif()
if(MW_BUILD_TOOLS AND APPLE)
    add_subdirectory(tools/mouse-accel-probe)
    add_subdirectory(tools/click-target)
endif()
# Linux: what the Vulkan Video chain of Phase 13 can count on, asked first.
if(MW_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(tools/vk-lab)
endif()
# Every OS: a game-like load on the encoder's GPU (needs Qt's qtshadertools).
if(MW_BUILD_TOOLS)
    add_subdirectory(tools/gpu-load)
    # The HID passthrough's prototype: a WebHID device recreated here (uhid).
    add_subdirectory(tools/hid-poc)
endif()
