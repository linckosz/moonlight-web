/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Backend unit-test runner. Aggregates the per-unit suites and exits non-zero
 * if any check fails, so it can gate CI / PR validation. Run under coverage via
 * tests/run_coverage.bat (OpenCppCoverage).
 */
#include "test_framework.h"

#include <QCoreApplication>
#include <cstdlib>

TestStats g_stats;

void run_input_encoder_tests();
void run_stream_config_tests();
void run_input_crypto_tests();
void run_rest_router_tests();
void run_app_settings_tests();
void run_launcher_detect_tests();
void run_host_app_image_tests();
void run_log_archive_tests();
void run_auth_manager_tests();
void run_connection_guard_tests();
void run_static_files_tests();
void run_home_screen_shell_tests();
void run_http_parser_tests();
void run_request_guard_tests();
void run_api_csrf_tests();
void run_session_pool_tests();
void run_port_fallback_tests();
void run_host_persistence_tests();
void run_rendezvous_id_tests();
void run_rendezvous_line_tests();
void run_address_race_tests();
void run_backend_probe_tests();
void run_host_os_probe_tests();
void run_send_backlog_tests();
void run_relay_frame_log_tests();
void run_ice_priority_tests();
void run_ice_udp_drop_tests();
void run_net_classify_tests();
void run_link_addresses_tests();
void run_audio_path_log_tests();
void run_packet_marking_tests();
void run_send_pacer_tests();
void run_mwsr_zone_block_tests();
void run_link_loss_tests();
void run_annexb_filler_tests();
void run_link_freeze_log_tests();
void run_host_uplink_tests();
void run_host_upload_cap_tests();
void run_reference_repairs_tests();
void run_rxbuf_section_tests();
void run_capture_key_tests();
void run_signaling_wait_tests();
void run_single_session_hosts_tests();
void run_stream_ends_tests();
void run_host_lag_tracker_tests();
void run_tunnel_frame_tests();
void run_tunnel_peer_policy_tests();
void run_app_manifest_tests();
void run_wolf_coop_tests();
void run_start_choice_tests();
void run_relaunch_script_tests();
void run_client_unique_id_tests();
void run_multiseat_tests();
void run_session_metrics_tests();
void run_us_scancode_tests();
void run_input_watchdog_tests();
void run_hid_collections_json_tests();
void run_hid_parked_json_tests();
void run_keyboard_layout_tests();
void run_native_capabilities_json_tests();
void run_edition_tests();
void run_mdns_advertiser_tests();
void run_router_port_core_tests();
void run_logger_rotation_tests();
void run_control_channel_tests();
void run_feed_ipc_tests();
void run_cert_manager_tests();
void run_mic_uplink_tests();

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Keep app data hermetic-ish for any path the units resolve internally.
    QCoreApplication::setApplicationName("mw-server-tests");
    QCoreApplication::setOrganizationName("moonlightweb");

    run_input_encoder_tests();
    run_stream_config_tests();
    run_input_crypto_tests();
    run_rest_router_tests();
    run_app_settings_tests();
    run_launcher_detect_tests();
    run_host_app_image_tests();
    run_log_archive_tests();
    run_auth_manager_tests();
    run_connection_guard_tests();
    run_static_files_tests();
    run_home_screen_shell_tests();
    run_http_parser_tests();
    run_request_guard_tests();
    run_api_csrf_tests();
    run_session_pool_tests();
    run_port_fallback_tests();
    run_host_persistence_tests();
    run_rendezvous_id_tests();
    run_rendezvous_line_tests();
    run_address_race_tests();
    run_backend_probe_tests();
    run_host_os_probe_tests();
    run_send_backlog_tests();
    run_relay_frame_log_tests();
    run_ice_priority_tests();
    run_ice_udp_drop_tests();
    run_net_classify_tests();
    run_link_addresses_tests();
    run_audio_path_log_tests();
    run_packet_marking_tests();
    run_send_pacer_tests();
    run_mwsr_zone_block_tests();
    run_link_loss_tests();
    run_annexb_filler_tests();
    run_link_freeze_log_tests();
    run_host_uplink_tests();
    run_host_upload_cap_tests();
    run_reference_repairs_tests();
    run_rxbuf_section_tests();
    run_capture_key_tests();
    run_signaling_wait_tests();
    run_single_session_hosts_tests();
    run_stream_ends_tests();
    run_host_lag_tracker_tests();
    run_tunnel_frame_tests();
    run_tunnel_peer_policy_tests();
    run_app_manifest_tests();
    run_wolf_coop_tests();
    run_start_choice_tests();
    run_relaunch_script_tests();
    run_client_unique_id_tests();
    run_multiseat_tests();
    run_session_metrics_tests();
    run_us_scancode_tests();
    run_input_watchdog_tests();
    run_hid_collections_json_tests();
    run_hid_parked_json_tests();
    run_keyboard_layout_tests();
    run_native_capabilities_json_tests();
    run_edition_tests();
    run_mdns_advertiser_tests();
    run_router_port_core_tests();
    run_control_channel_tests();
    run_feed_ipc_tests();
    run_cert_manager_tests();
    run_mic_uplink_tests();
    // Last: it repoints the Logger singleton at its own temp files.
    run_logger_rotation_tests();

    const int total = g_stats.passed + g_stats.failed;
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "Backend TNR: %d/%d checks passed, %d failed\n", g_stats.passed, total,
                 g_stats.failed);
    std::fprintf(stderr, "========================================\n");

    // Exit via _Exit so the process return code is the test result and is not
    // clobbered by a crash in global/Qt/OpenSSL teardown after main() returns.
    const int rc = g_stats.failed == 0 ? 0 : 1;
    std::fflush(stderr);
    std::_Exit(rc);
}
