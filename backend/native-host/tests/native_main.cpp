/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 *
 * Runs the engine's pure-logic units. No Qt, no GPU, no display: everything
 * here is verifiable on a headless CI runner, which is the point — the policy
 * that spares the user fifty settings (which GPU, which encoder, which codec,
 * what resolution) is exactly the part that must never silently drift.
 */
#include "native_test_framework.h"

#include "mw/native/NativeHost.h"

#include <cstdlib>
#include <cstring>
#include <string>

NativeTestStats g_nativeStats;

void run_selector_tests();
void run_capabilities_tests();
void run_capture_tests();
void run_color_convert_tests();
void run_color_convert12_tests();
void run_color_convert12_gpu_tests();
void run_d3d11_video_pipeline_tests();
void run_d3d12_device_tests();
void run_dda_interop_tests();
void run_video_encode_caps12_tests();
void run_video_encode12_tests();
void run_encoder_configs_tests();
void run_vendor_encode12_tests();
void run_win32_cursor_tests();
void run_win_text_keys_tests();
void run_host_mute_tests();
void run_evdev_keymap_tests();
void run_pointer_clamp_tests();
void run_recentre_detector_tests();
void run_wheel_carry_tests();
void run_xkb_text_map_tests();
void run_absolute_map_tests();
void run_wayland_layout_tests();
void run_monitor_layout_tests();
void run_gamescope_tests();
void run_x11_layout_tests();
void run_scanout_window_tests();
void run_linux_pipeline_tests();
void run_vulkan_convert_tests();
void run_vulkan_hevc_tests();
void run_vulkan_av1_tests();
void run_cpu_cursor_tests();
void run_gl_readback_tests();
void run_linux_cpu_fallback_tests();
void run_portal_tests();
void run_encode_load_cap_tests();
void run_h264_vui_tests();
void run_parameter_sets_tests();
void run_hevc_slice_parser_tests();
void run_hevc_dpb_tests();
void run_hevc_negotiation_tests();
void run_h264_slice_parser_tests();
void run_h264_negotiation_tests();
void run_av1_obu_tests();
void run_video_pipeline_choice_tests();
void run_d3d12_fault_tests();
void run_linux_route_choice_tests();
void run_linux_session_tests();
void run_linux_virtual_display_tests();
void run_mac_keymap_tests();
void run_mac_session_tests();
void run_vpl_params_tests();
void run_openh264_tests();
void run_bgra_to_i420_tests();
void run_ds4_mapping_tests();
void run_hid_descriptor_tests();
void run_hid_passthrough_tests();
void run_hid_pid_tests();
void run_hid_parking_tests();
void run_hid_evdev_tests();
void run_stage_stats_tests();
void run_frame_cadence_tests();
void run_cadence_lock_tests();
void run_input_admit_tests();
void run_capture_hold_tests();
void run_resample_cost_tests();
void run_mwsr_zones_tests();
void run_cadence_align_tests();
void run_cadence_choice_tests();
void run_cadence_step_tests();
void run_decode_credit_tests();
void run_deadline_cadence_tests();
void run_restart_backoff_tests();
void run_click_trace_tests();
void run_rate_control_tests();
void run_qp_rate_controller_tests();
void run_virtual_display_tests();
void run_reference_slots_tests();
void run_intra_refresh_sweep_tests();
void run_audio_pacer_tests();
void run_audio_interleave_tests();
void run_cursor_blend_tests();
void run_frame_fit_tests();
void run_painted_pointer_tests();
void run_output_match_tests();
void run_display_mode_hold_tests();
void run_cursor_position_gate_tests();
void run_pointer_presence_tests();
void run_system_mouse_tests();
void run_feed_header_tests();
void run_feed_arbiter_tests();

void installTestLogSink()
{
    mw::native::NativeHost::setLogSink([](int level, const std::string& message) {
        static const char* kNames[] = {"debug", "info", "warn", "error"};
        const char* name = (level >= 0 && level <= 3) ? kNames[level] : "?";
        std::fprintf(stderr, "  [%s] %s\n", name, message.c_str());
    });
}

namespace {

int g_argc = 0;
char** g_argv = nullptr;

/// With group names on the command line (`mw-native-tests video_encode12
/// qp_rate_controller`), only those run; with none, all of them. The hardware
/// groups capture displays and open encoders: a run of the pure logic alone
/// leaves a machine that is streaming alone.
bool selected(const char* group)
{
    if (g_argc <= 1) return true;
    for (int i = 1; i < g_argc; ++i)
        if (std::strcmp(g_argv[i], group) == 0) return true;
    return false;
}

} // namespace

#define RUN(group)                                                                                 \
    if (selected(#group)) run_##group##_tests()

int main(int argc, char** argv)
{
    g_argc = argc;
    g_argv = argv;
    // Route the engine's own logging to stderr for the whole run. The engine
    // explains itself in the log — which adapter refused a session, why a
    // driver was rejected — and a suite that hides that leaves a failure with
    // nothing to go on but a count.
    installTestLogSink();

    RUN(capabilities);
    RUN(selector);
    RUN(encode_load_cap);
    RUN(h264_vui);
    RUN(parameter_sets);
    RUN(hevc_slice_parser);
    RUN(hevc_dpb);
    RUN(hevc_negotiation);
    RUN(h264_slice_parser);
    RUN(h264_negotiation);
    RUN(av1_obu);
    RUN(video_pipeline_choice);
    RUN(d3d12_fault);
    RUN(linux_route_choice);
    RUN(vpl_params);
#ifdef MW_NATIVE_OPENH264
    RUN(openh264);
#endif
    RUN(bgra_to_i420);
    RUN(ds4_mapping);
    RUN(hid_descriptor);
    RUN(hid_passthrough);
    RUN(hid_pid);
    RUN(hid_parking);
    RUN(hid_evdev);
    RUN(stage_stats);
    RUN(frame_cadence);
    RUN(cadence_lock);
    RUN(input_admit);
    RUN(capture_hold);
    RUN(resample_cost);
    RUN(mwsr_zones);
    RUN(cadence_align);
    RUN(cadence_choice);
    RUN(cadence_step);
    RUN(decode_credit);
    RUN(deadline_cadence);
    RUN(restart_backoff);
    RUN(click_trace);
    RUN(rate_control);
    RUN(qp_rate_controller);
    RUN(virtual_display);
    RUN(reference_slots);
    RUN(intra_refresh_sweep);
    RUN(audio_pacer);
    RUN(audio_interleave);
    RUN(cursor_blend);
    RUN(frame_fit);
    RUN(painted_pointer);
    RUN(output_match);
    RUN(display_mode_hold);
    RUN(cursor_position_gate);
    RUN(pointer_presence);
    RUN(system_mouse);
    RUN(feed_header);
    RUN(feed_arbiter);
    RUN(capture);
    RUN(color_convert);
    RUN(color_convert12);
    RUN(d3d11_video_pipeline);
    RUN(d3d12_device);
    RUN(dda_interop);
    RUN(video_encode_caps12);
    RUN(video_encode12);
    RUN(encoder_configs);
    RUN(vendor_encode12);
    // Last of the D3D12 groups: a GPU that hangs on it takes the process's
    // D3D12 device on that GPU with it.
    RUN(color_convert12_gpu);
    RUN(win32_cursor);
    RUN(win_text_keys);
    RUN(host_mute);
    RUN(evdev_keymap);
    RUN(pointer_clamp);
    RUN(recentre_detector);
    RUN(wheel_carry);
    RUN(xkb_text_map);
    RUN(absolute_map);
    RUN(wayland_layout);
    RUN(monitor_layout);
    RUN(gamescope);
    RUN(x11_layout);
    RUN(scanout_window);
    RUN(mac_keymap);
    RUN(linux_pipeline);
    RUN(vulkan_convert);
    RUN(vulkan_hevc);
    RUN(vulkan_av1);
    RUN(cpu_cursor);
    RUN(gl_readback);
    RUN(portal);
    RUN(linux_session);
    RUN(linux_cpu_fallback);
    RUN(linux_virtual_display);
    RUN(mac_session);

    const int total = g_nativeStats.passed + g_nativeStats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "native-host: %d/%d checks passed, %d failed\n", g_nativeStats.passed,
                 total, g_nativeStats.failed);
    std::fprintf(stderr, "========================================\n");
    std::fflush(stderr);

    return g_nativeStats.failed == 0 ? 0 : 1;
}
