/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "test_framework.h"
#include "../packaging/linux/moonlightweb-launch-env.h"
#include "common/LinuxCapabilities.h"

#include <string>
#include <vector>

namespace {

int modeOf(std::vector<const char*> args)
{
    std::vector<char*> argv;
    for (const char* a : args)
        argv.push_back(const_cast<char*>(a));
    argv.push_back(nullptr);
    return mw_launch_mode(static_cast<int>(args.size()), argv.data());
}

// The environment a handed mode gets from @p in, as strings.
std::vector<std::string> built(std::vector<const char*> in, int* count = nullptr)
{
    std::vector<char*> envp;
    for (const char* e : in)
        envp.push_back(const_cast<char*>(e));
    envp.push_back(nullptr);
    std::vector<const char*> out(in.size() + MW_LAUNCH_ENV_EXTRA, nullptr);
    char home[512];
    char xauth[512];
    const int n = mw_launch_env(envp.data(), out.data(), static_cast<int>(out.size()), home, xauth,
                                sizeof xauth);
    if (count) *count = n;
    std::vector<std::string> result;
    for (int i = 0; n > 0 && i < n; ++i)
        result.emplace_back(out[i]);
    if (n >= 0 && out[n] != nullptr) result.emplace_back("<not NULL-terminated>");
    return result;
}

bool has(const std::vector<std::string>& env, const std::string& entry)
{
    for (const std::string& e : env)
        if (e == entry) return true;
    return false;
}

bool hasName(const std::vector<std::string>& env, const std::string& name)
{
    for (const std::string& e : env)
        if (e.compare(0, name.size() + 1, name + "=") == 0) return true;
    return false;
}

} // namespace

// moonlightweb-launch hands CAP_SYS_NICE to the stream worker only, and starts
// it from a known environment.
void run_launch_env_tests()
{
    SECTION("Launcher: which start is handed the capability");

    CHECK_EQ(modeOf({"moonlightweb-launch"}), static_cast<int>(MW_LAUNCH_APP));
    CHECK_EQ(modeOf({"moonlightweb-launch", "--stream-worker"}),
             static_cast<int>(MW_LAUNCH_WORKER));
    CHECK_EQ(modeOf({"moonlightweb-launch", "--stream-worker", "--dev"}),
             static_cast<int>(MW_LAUNCH_WORKER));
    // The probe runs in the app, which holds nothing; through here it is the app.
    CHECK_EQ(modeOf({"moonlightweb-launch", "--native-probe"}), static_cast<int>(MW_LAUNCH_APP));
    // The first argument, exactly: anything else is the app.
    CHECK_EQ(modeOf({"moonlightweb-launch", "--autostart"}), static_cast<int>(MW_LAUNCH_APP));
    CHECK_EQ(modeOf({"moonlightweb-launch", "--dev", "--stream-worker"}),
             static_cast<int>(MW_LAUNCH_APP));
    CHECK_EQ(modeOf({"moonlightweb-launch", "--stream-worker=1"}), static_cast<int>(MW_LAUNCH_APP));
    CHECK_EQ(modeOf({"moonlightweb-launch", "--STREAM-WORKER"}), static_cast<int>(MW_LAUNCH_APP));
    CHECK_EQ(modeOf({"moonlightweb-launch", "-stream-worker"}), static_cast<int>(MW_LAUNCH_APP));
    CHECK_EQ(modeOf({"moonlightweb-launch", ""}), static_cast<int>(MW_LAUNCH_APP));

    SECTION("Launcher: the environment of a handed mode");

    // Kept by exact name or by prefix, with a value.
    CHECK(mw_launch_env_kept("USER=bruno"));
    CHECK(mw_launch_env_kept("WAYLAND_DISPLAY=wayland-0"));
    CHECK(mw_launch_env_kept("XAUTHORITY=/run/user/1000/gdm/Xauthority"));
    CHECK(mw_launch_env_kept("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus"));
    CHECK(mw_launch_env_kept("MW_LAN_ONLY=1"));
    CHECK(mw_launch_env_kept("MW_SERVICE=1"));
    CHECK(mw_launch_env_kept("LC_ALL=C"));
    CHECK(mw_launch_env_kept("XDG_SESSION_TYPE=x11"));
    CHECK(mw_launch_env_kept("NOTIFY_SOCKET=/run/systemd/notify"));
    CHECK(mw_launch_env_kept("MW_LAN_ONLY="));

    // The locale and the time zone: names, kept; paths, dropped.
    CHECK(mw_launch_env_kept("LANG=fr_FR.UTF-8"));
    CHECK(mw_launch_env_kept("LC_ALL=C.UTF-8"));
    CHECK(mw_launch_env_kept("LC_TIME=en_GB.UTF-8"));
    CHECK(mw_launch_env_kept("LANGUAGE=fr:en"));
    CHECK(mw_launch_env_kept("TZ=Europe/Paris"));
    CHECK(mw_launch_env_kept("TZ=:Europe/Paris"));
    CHECK(mw_launch_env_kept("TZ=UTC"));
    CHECK(mw_launch_env_kept("TZ="));
    CHECK(!mw_launch_env_kept("LANG=/tmp/x"));
    CHECK(!mw_launch_env_kept("LANG=../x"));
    CHECK(!mw_launch_env_kept("LC_ALL=../x"));
    CHECK(!mw_launch_env_kept("LC_MESSAGES=/tmp/x"));
    CHECK(!mw_launch_env_kept("LC_CTYPE=fr_FR..x"));
    CHECK(!mw_launch_env_kept("LANGUAGE=fr:/tmp/x"));
    CHECK(!mw_launch_env_kept("LANGUAGE=../../tmp/x"));
    CHECK(!mw_launch_env_kept("TZ=/tmp/x"));
    CHECK(!mw_launch_env_kept("TZ=:/tmp/x"));
    CHECK(!mw_launch_env_kept("TZ=../x"));
    CHECK(!mw_launch_env_kept("TZ=Europe/../../tmp/x"));
    CHECK(!hasName(built({"LANG=/tmp/x", "TZ=:/tmp/x"}), "LANG"));
    CHECK(!hasName(built({"LANG=/tmp/x", "TZ=:/tmp/x"}), "TZ"));
    CHECK(has(built({"LANG=fr_FR.UTF-8", "TZ=Europe/Paris"}), "TZ=Europe/Paris"));

    // Not kept: no exact name, a longer or shorter name, no value at all.
    CHECK(!mw_launch_env_kept("USERX=1"));
    CHECK(!mw_launch_env_kept("XUSER=1"));
    CHECK(!mw_launch_env_kept("USER"));
    CHECK(!mw_launch_env_kept("LC_=x"));
    CHECK(!mw_launch_env_kept("MW_LAN_ONLYX=1"));
    CHECK(!mw_launch_env_kept("MW_VK_PROOF_CACHE=0"));
    CHECK(!mw_launch_env_kept("MW_UNKNOWN=1"));
    CHECK(!mw_launch_env_kept("LD_PRELOAD=/tmp/x.so"));
    CHECK(!mw_launch_env_kept("LD_LIBRARY_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("OPENSSL_CONF=/tmp/x.cnf"));
    CHECK(!mw_launch_env_kept("OPENSSL_MODULES=/tmp"));
    CHECK(!mw_launch_env_kept("SSL_CERT_FILE=/tmp/x.pem"));
    CHECK(!mw_launch_env_kept("QT_PLUGIN_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("QT_QPA_PLATFORM_PLUGIN_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("QT_QPA_PLATFORMTHEME=x"));
    CHECK(!mw_launch_env_kept("VK_ADD_LAYER_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("VK_ICD_FILENAMES=/tmp/x.json"));
    CHECK(!mw_launch_env_kept("VK_DRIVER_FILES=/tmp/x.json"));
    CHECK(!mw_launch_env_kept("VK_INSTANCE_LAYERS=x"));
    CHECK(!mw_launch_env_kept("LIBVA_DRIVERS_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("LIBVA_DRIVER_NAME=x"));
    CHECK(!mw_launch_env_kept("LIBGL_DRIVERS_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("__EGL_VENDOR_LIBRARY_FILENAMES=/tmp/x.json"));
    CHECK(!mw_launch_env_kept("GBM_BACKENDS_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("MESA_LOADER_DRIVER_OVERRIDE=x"));
    CHECK(!mw_launch_env_kept("GST_PLUGIN_PATH=/tmp"));
    CHECK(!mw_launch_env_kept("GIO_MODULE_DIR=/tmp"));
    CHECK(!mw_launch_env_kept("PIPEWIRE_MODULE_DIR=/tmp"));
    CHECK(!mw_launch_env_kept("SPA_PLUGIN_DIR=/tmp"));
    CHECK(!mw_launch_env_kept("ALSA_CONFIG_PATH=/tmp/x.conf"));
    CHECK(!mw_launch_env_kept("HOME=/home/u"));
    CHECK(!mw_launch_env_kept("PATH=/home/u/bin:/usr/bin"));
    CHECK(!mw_launch_env_kept("XDG_DATA_HOME=/tmp"));
    CHECK(!mw_launch_env_kept("XDG_CONFIG_DIRS=/tmp"));

    // A whole environment: the kept entries, the fixed ones, nothing else.
    int n = -2;
    const std::vector<std::string> env =
        built({"HOME=/home/u", "PATH=/home/u/bin:/usr/bin", "USER=u", "LD_PRELOAD=/tmp/x.so",
               "OPENSSL_CONF=/tmp/x.cnf", "XDG_DATA_HOME=/tmp/d", "XDG_DATA_DIRS=/tmp/dd",
               "MW_LAN_ONLY=1", "WAYLAND_DISPLAY=wayland-0", "LC_TIME=fr_FR.UTF-8"},
              &n);
    CHECK(n > 0);
    CHECK(has(env, "USER=u"));
    CHECK(has(env, "MW_LAN_ONLY=1"));
    CHECK(has(env, "WAYLAND_DISPLAY=wayland-0"));
    CHECK(has(env, "LC_TIME=fr_FR.UTF-8"));
    CHECK(has(env, "HOME=/nonexistent"));
    CHECK(has(env, "XDG_CONFIG_HOME=/nonexistent"));
    CHECK(has(env, "XDG_DATA_HOME=/nonexistent"));
    CHECK(has(env, "XDG_CACHE_HOME=/nonexistent"));
    CHECK(has(env, "XDG_STATE_HOME=/nonexistent"));
    CHECK(has(env, "XDG_CONFIG_DIRS=/etc/xdg"));
    CHECK(has(env, "XDG_DATA_DIRS=/usr/local/share:/usr/share"));
    CHECK(has(env, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin:"
                   "/var/lib/flatpak/exports/bin"));
    CHECK(!has(env, "HOME=/home/u"));
    CHECK(!has(env, "PATH=/home/u/bin:/usr/bin"));
    CHECK(!has(env, "XDG_DATA_HOME=/tmp/d"));
    CHECK(!has(env, "XDG_DATA_DIRS=/tmp/dd"));
    CHECK(!hasName(env, "LD_PRELOAD"));
    CHECK(!hasName(env, "OPENSSL_CONF"));
    // Each name once: what was fixed is not also there as the caller had it.
    int homes = 0;
    for (const std::string& e : env)
        if (e.compare(0, 5, "HOME=") == 0) ++homes;
    CHECK_EQ(homes, 1);
    CHECK(!has(env, "<not NULL-terminated>"));

    // X's cookie: from the caller's home when XAUTHORITY was not set, as is
    // when it was, and nowhere when there was no absolute home.
    CHECK(has(built({"HOME=/home/u", "DISPLAY=:0"}), "XAUTHORITY=/home/u/.Xauthority"));
    const std::vector<std::string> gdm =
        built({"HOME=/home/u", "XAUTHORITY=/run/user/1000/gdm/Xauthority"});
    CHECK(has(gdm, "XAUTHORITY=/run/user/1000/gdm/Xauthority"));
    CHECK(!has(gdm, "XAUTHORITY=/home/u/.Xauthority"));
    CHECK(!hasName(built({"DISPLAY=:0"}), "XAUTHORITY"));
    CHECK(!hasName(built({"HOME=relative", "DISPLAY=:0"}), "XAUTHORITY"));

    // Where the app's own code finds the user's data: the caller's home, by a
    // name no library reads — derived, never taken from the caller as is.
    CHECK(has(built({"HOME=/home/u"}), "MW_USER_HOME=/home/u"));
    const std::vector<std::string> forged = built({"HOME=/home/u", "MW_USER_HOME=/tmp/elsewhere"});
    CHECK(has(forged, "MW_USER_HOME=/home/u"));
    CHECK(!has(forged, "MW_USER_HOME=/tmp/elsewhere"));
    CHECK(!mw_launch_env_kept("MW_USER_HOME=/tmp/elsewhere"));
    CHECK(!hasName(built({"MW_USER_HOME=/tmp/elsewhere"}), "MW_USER_HOME"));
    CHECK(!hasName(built({"HOME=relative"}), "MW_USER_HOME"));
    int users = 0;
    for (const std::string& e : forged)
        if (e.compare(0, 13, "MW_USER_HOME=") == 0) ++users;
    CHECK_EQ(users, 1);

    // An empty environment still gets the fixed entries.
    int empty = -2;
    const std::vector<std::string> none = built({}, &empty);
    CHECK_EQ(empty, static_cast<int>(MW_LAUNCH_COUNT(mw_launch_env_fixed)));
    CHECK(has(none, "HOME=/nonexistent"));

    // Too little room is an error, never a truncated environment.
    {
        char* in[] = {const_cast<char*>("USER=u"), const_cast<char*>("HOME=/home/u"), nullptr};
        const char* out[3] = {};
        char home[64];
        char xauth[64];
        CHECK_EQ(mw_launch_env(in, out, 3, home, xauth, sizeof xauth), -1);
        const char* roomy[32] = {};
        char tinyHome[8];
        char tinyXauth[8];
        CHECK_EQ(mw_launch_env(in, roomy, 32, tinyHome, tinyXauth, sizeof tinyXauth), -1);
        CHECK(mw_launch_env(in, roomy, 32, home, xauth, sizeof xauth) > 0);
    }

    SECTION("Launcher: its file capability, read back");

    // What `setcap cap_sys_nice+p` writes on the launcher (revision 2): the
    // magic, then permitted and inheritable for capabilities 0-31 and 32-63.
    const unsigned char package[20] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x80, 0x00, 0, 0,
                                       0,    0,    0,    0,    0,    0,    0,    0,    0, 0};
    constexpr int kSysAdmin = 21, kSysNice = 23, kNetAdmin = 12;
    CHECK(mw::fileCapabilityGrants(package, sizeof package, kSysNice));
    CHECK(!mw::fileCapabilityGrants(package, sizeof package, kSysAdmin));
    CHECK(!mw::fileCapabilityGrants(package, sizeof package, kNetAdmin));
    // `cap_sys_admin+p`, moonlightweb-kms's: no priority to hand a worker.
    const unsigned char adminOnly[20] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x20, 0x00, 0, 0,
                                         0,    0,    0,    0,    0,    0,    0,    0,    0, 0};
    CHECK(mw::fileCapabilityGrants(adminOnly, sizeof adminOnly, kSysAdmin));
    CHECK(!mw::fileCapabilityGrants(adminOnly, sizeof adminOnly, kSysNice));
    // Both, one word: each read on its own bit.
    const unsigned char both[20] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xA0, 0x00, 0, 0,
                                    0,    0,    0,    0,    0,    0,    0,    0,    0, 0};
    CHECK(mw::fileCapabilityGrants(both, sizeof both, kSysAdmin));
    CHECK(mw::fileCapabilityGrants(both, sizeof both, kSysNice));
    // Inheritable is not permitted.
    const unsigned char inheritable[20] = {0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0, 0x00, 0x00,
                                           0xA0, 0x00, 0,    0,    0, 0, 0, 0, 0,    0};
    CHECK(!mw::fileCapabilityGrants(inheritable, sizeof inheritable, kSysAdmin));
    // Revision 3 (a namespaced root id after the data), revision 1 (32 bits).
    const unsigned char rev3[24] = {0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x20, 0x00, 0, 0, 0, 0,
                                    0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0};
    CHECK(mw::fileCapabilityGrants(rev3, sizeof rev3, kSysAdmin));
    const unsigned char rev1[12] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x20, 0x00, 0, 0, 0, 0};
    CHECK(mw::fileCapabilityGrants(rev1, sizeof rev1, kSysAdmin));
    CHECK(!mw::fileCapabilityGrants(rev1, sizeof rev1, 40));
    // The upper word: capability 40 is bit 8 of the second permitted word.
    const unsigned char upper[20] = {0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0, 0, 0,
                                     0,    0,    0x00, 0x01, 0, 0, 0, 0, 0, 0};
    CHECK(mw::fileCapabilityGrants(upper, sizeof upper, 40));
    CHECK(!mw::fileCapabilityGrants(upper, sizeof upper, kSysAdmin));
    // Truncated, unknown, empty: nothing granted.
    CHECK(!mw::fileCapabilityGrants(package, 16, kSysNice));
    CHECK(!mw::fileCapabilityGrants(rev3, 20, kSysAdmin));
    const unsigned char unknown[20] = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x20, 0x00, 0, 0,
                                       0,    0,    0,    0,    0,    0,    0,    0,    0, 0};
    CHECK(!mw::fileCapabilityGrants(unknown, sizeof unknown, kSysAdmin));
    CHECK(!mw::fileCapabilityGrants(nullptr, 20, kSysAdmin));
    CHECK(!mw::fileCapabilityGrants(package, 3, kSysNice));
    CHECK(!mw::fileCapabilityGrants(package, sizeof package, -1));
}
