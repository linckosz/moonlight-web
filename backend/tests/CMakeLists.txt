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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network WebSockets)
set(CMAKE_AUTOMOC ON)

# ── Test runner ─────────────────────────────────────────────────────────────
add_executable(run_tests
    # Test sources
    main.cpp
    test_input_encoder.cpp
    test_hid_collections_json.cpp
    test_hid_parked_json.cpp
    test_stream_config.cpp
    test_input_crypto.cpp
    test_rest_router.cpp
    test_app_settings.cpp
    test_launcher_detect.cpp
    test_host_app_image.cpp
    test_log_archive.cpp
    test_auth_manager.cpp
    test_connection_guard.cpp
    test_static_files.cpp
    test_home_screen_shell.cpp
    test_http_parser.cpp
    test_request_guard.cpp
    test_api_csrf.cpp
    test_session_pool.cpp
    test_port_fallback.cpp
    test_host_persistence.cpp
    test_rendezvous_id.cpp
    test_rendezvous_line.cpp
    test_address_race.cpp
    test_backend_probe.cpp
    test_host_os_probe.cpp
    test_send_backlog.cpp
    test_relay_frame_log.cpp
    test_ice_priority.cpp
    test_ice_udp_drop.cpp
    test_net_classify.cpp
    test_link_addresses.cpp
    test_audio_path_log.cpp
    test_packet_marking.cpp
    test_send_pacer.cpp
    test_mwsr_zone_block.cpp
    test_link_loss.cpp
    test_annexb_filler.cpp
    test_link_freeze_log.cpp
    test_host_uplink.cpp
    test_host_upload_cap.cpp
    test_reference_repairs.cpp
    test_rxbuf_section.cpp
    test_capture_key.cpp
    test_signaling_wait.cpp
    test_single_session_hosts.cpp
    test_stream_ends.cpp
    test_host_lag_tracker.cpp
    test_tunnel_frame.cpp
    test_tunnel_peer_policy.cpp
    test_app_manifest.cpp
    test_wolf_coop.cpp
    test_start_choice.cpp
    test_relaunch_script.cpp
    test_client_unique_id.cpp
    test_multiseat.cpp
    test_session_metrics.cpp
    test_us_scancode.cpp
    test_input_watchdog.cpp
    test_keyboard_layout.cpp
    test_native_capabilities_json.cpp
    test_edition.cpp
    test_mdns_advertiser.cpp
    test_router_port_core.cpp
    test_logger_rotation.cpp
    test_control_channel.cpp
    test_feed_ipc.cpp
    test_cert_manager.cpp
    # In-scope production units (+ their real deps)
    # The router-port allocation policy, behind its gateway seam: the rules
    # that keep two MoonlightWeb hosts on one LAN out of each other's holes.
    ${_src}/src/network/RouterPortCore.cpp
    # Which MoonlightWeb this is: the names, ports and version a DEV build
    # must answer to so it can sit beside a production install.
    ${_src}/src/common/Edition.cpp
    ${_src}/src/network/MdnsAdvertiser.cpp
    # The probe result as it crosses from the console session to the service.
    # Capabilities.cpp is the engine's plain-C++ enum/lookup file: no Windows
    # header, no D3D, so it comes along on any platform.
    ${_src}/src/backend/streambackend/NativeCapabilitiesJson.cpp
    ${_src}/native-host/src/core/Capabilities.cpp
    ${_src}/src/streaming/InputEncoder.cpp
    # The input dead-man switch, exercised against a fake wire.
    ${_src}/src/streaming/InputWatchdog.cpp
    # The adapter a stream leaves by and its counter: read-only system calls.
    ${_src}/src/streaming/HostUplinkReader.cpp
    # The bench's cap on the host's other uploads: its scripts, built and read.
    ${_src}/src/streaming/HostUploadCap.cpp
    # The HID passthrough's hidattach, read and rebuilt into a descriptor.
    ${_src}/src/streaming/HidCollectionsJson.cpp
    # Its wheels kept between two streams, as they travel between processes.
    ${_src}/src/streaming/HidParkedJson.cpp
    ${_src}/native-host/src/input/HidDescriptor.cpp
    ${_src}/src/streaming/StreamConfig.cpp
    ${_src}/src/streaming/InputCrypto.cpp
    ${_src}/src/server/RestRouter.cpp
    ${_src}/src/server/AppSettings.cpp
    ${_src}/src/server/AuthManager.cpp
    # The local self-signed cert: its key outlives a restart (clients pin it).
    ${_src}/src/server/CertManager.cpp
    ${_src}/src/server/ConnectionGuard.cpp
    ${_src}/src/server/StaticFileHandler.cpp
    ${_src}/src/server/HomeScreenShell.cpp
    ${_src}/src/server/HttpParser.cpp
    ${_src}/src/server/RequestGuard.cpp
    ${_src}/src/server/SessionPool.cpp
    # The single-tab control channel: the port it lands on when its own is taken.
    ${_src}/src/server/ControlChannel.cpp
    ${_src}/src/common/RendezvousId.cpp
    # The host's line to the introduction server, against a local stand-in:
    # how fast it comes back when the line fails under it.
    ${_src}/src/network/RendezvousClient.cpp
    # Which of the server's addresses the line opens on: the race behind it,
    # with legs the test answers by hand.
    ${_src}/src/network/AddressRace.cpp
    # The session location lookup: the request it sends, built and never sent.
    ${_src}/src/network/GeoIpService.cpp
    # Host persistence: the restart path for a paired host (issue #9).
    ${_src}/src/backend/NvComputer.cpp
    ${_src}/src/backend/NvHTTP.cpp
    # Backend detection: what a host runs, worked out rather than asked for.
    ${_src}/src/backend/streambackend/BackendProbe.cpp
    # And which OS it runs, which decides whether scroll has to be quantized.
    ${_src}/src/backend/streambackend/HostOsProbe.cpp
    # Wolf's control API: the wire shapes native co-op depends on. The selection
    # rules themselves are header-only (WolfCoop.h), so nothing else comes in.
    ${_src}/src/backend/WolfApiClient.cpp
    # MultiSeat's control API: how a seat reads off the wire, and whether it can
    # be streamed. Both are pure enough to test without the service.
    ${_src}/src/backend/MultiSeatApiClient.cpp
    ${_src}/src/backend/IdentityManager.cpp
    # AppSettings and AuthManager both reach into MW-BIND-v1 now: the host
    # identity key, and the pairing key bound to a session.
    ${_src}/src/common/PairingCrypto.cpp
    # The session census: what leaves the machine, and what must not.
    ${_src}/src/network/SessionMetrics.cpp
    # The guests' shared feed: the pipe between the feed worker and theirs.
    ${_src}/src/streaming/FeedInfo.cpp
    ${_src}/src/streaming/FeedPublisher.cpp
    ${_src}/src/streaming/FeedSubscriber.cpp
    ${_src}/src/common/Logger.cpp
    # The "Download logs" archive: which files go in, and the .zip they go in.
    ${_src}/src/common/ZipWriter.cpp
    ${_src}/src/server/LogArchive.cpp
    ${_src}/src/server/LogScrubber.cpp
    # The native host's apps: the launchers found on a PC, and their cards' images.
    ${_src}/src/server/LauncherDetect.cpp
    ${_src}/src/server/HostAppImage.cpp
)

target_include_directories(run_tests PRIVATE
    ${_src}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Limelight.h (header-only macros used by StreamConfig).
    ${_src}/third_party/moonlight-common-c/src
    # The native host's US keyboard table. Header-only and free of windows.h,
    # so the layout the injector converts through can be checked on any
    # platform — the rest of that engine cannot come along for the ride.
    ${_src}/native-host/src
    # The engine's public Capabilities.h, for the JSON round trip above.
    ${_src}/native-host/include
)

# Silence production qDebug() noise; keep qInfo()/qCritical() (test output).
target_compile_definitions(run_tests PRIVATE QT_NO_DEBUG_OUTPUT)
# CertManager's public-cert search paths, as in the server build. The tests only
# exercise the local cert, which lives in the runner's AppData.
target_compile_definitions(run_tests PRIVATE
    FRONTEND_DIR="${_src}/../frontend/"
    CERT_DIR="${_src}/cert/"
)

target_link_libraries(run_tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets
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

# ── Passive ICE-TCP: our patch on libjuice ─────────────────────────────────
# A runner of its own, in C against libjuice itself: the patch lives in the
# library, below anything run_tests links. The backend's configure applies the
# patch to the submodule; a test tree configured alone applies it the same way
# (once, left alone when it already is). docs/design/ice-tcp-passive.md
find_package(Git REQUIRED)
set(_juice ${_src}/third_party/libdatachannel/deps/libjuice)
set(_juice_patch ${_src}/third_party/patches/libjuice-ice-tcp-passive.patch)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_juice}" apply --reverse --check "${_juice_patch}"
                RESULT_VARIABLE _juice_patched OUTPUT_QUIET ERROR_QUIET)
if(NOT _juice_patched EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_juice}" apply "${_juice_patch}"
                    RESULT_VARIABLE _juice_failed ERROR_VARIABLE _juice_why)
    if(NOT _juice_failed EQUAL 0)
        message(FATAL_ERROR "libjuice-ice-tcp-passive.patch does not apply: ${_juice_why}")
    endif()
endif()
set(NO_SERVER ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(${_juice} ${CMAKE_BINARY_DIR}/libjuice EXCLUDE_FROM_ALL)

add_executable(run_juice_tests test_ice_tcp_passive.c)
# Its private headers: the test writes STUN messages with the library's own code.
target_include_directories(run_juice_tests PRIVATE ${_juice}/src ${_juice}/include/juice)
target_link_libraries(run_juice_tests PRIVATE juice-static)

enable_testing()
add_test(NAME backend-tnr COMMAND run_tests)
add_test(NAME ice-tcp-passive COMMAND run_juice_tests)
