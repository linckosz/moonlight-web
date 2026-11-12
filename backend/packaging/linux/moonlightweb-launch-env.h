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

/*
 * What moonlightweb-launch starts, and with which environment — pure C, no
 * system call, so the backend's tests run it on every platform.
 *
 * Only two internal modes are handed the capabilities: the stream worker and
 * the native probe, named by the FIRST argument exactly, as the app spawns
 * them. Every other start — the app itself, from the menu, the autostart entry
 * or the systemd unit — is handed none.
 *
 * A mode that is handed one starts from a known environment rather than the
 * caller's: the names it uses (the session it draws on, the locale, the
 * service manager's, the app's own MW_* switches) and nothing else, with the
 * home and search directories fixed. What a worker needs from the user's home
 * (its settings file, its log folder) the app passes on its command line and
 * its config line instead, and what the app's own code reads there as data
 * comes under MW_USER_HOME (mw_launch_env).
 */

#ifndef MOONLIGHTWEB_LAUNCH_ENV_H
#define MOONLIGHTWEB_LAUNCH_ENV_H

#include <stddef.h>
#include <string.h>

enum
{
    MW_LAUNCH_APP = 0,    /* anything else: handed nothing */
    MW_LAUNCH_WORKER = 1, /* --stream-worker */
    MW_LAUNCH_PROBE = 2   /* --native-probe */
};

static inline int mw_launch_mode(int argc, char* const* argv)
{
    if (argc < 2 || !argv[1]) return MW_LAUNCH_APP;
    if (strcmp(argv[1], "--stream-worker") == 0) return MW_LAUNCH_WORKER;
    if (strcmp(argv[1], "--native-probe") == 0) return MW_LAUNCH_PROBE;
    return MW_LAUNCH_APP;
}

/* Kept as the caller has them, by exact name. */
static const char* const mw_launch_env_names[] = {
    "USER", "LOGNAME", "LANG", "LANGUAGE", "TZ",
    /* the session the worker captures, injects into and plays to */
    "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "XDG_SEAT",
    "XDG_CURRENT_DESKTOP", "DBUS_SESSION_BUS_ADDRESS",
    /* the service manager's, under the systemd unit */
    "NOTIFY_SOCKET", "INVOCATION_ID", "JOURNAL_STREAM", "LISTEN_PID", "LISTEN_FDS",
    "LISTEN_FDNAMES",
    /* the app's own switches: flags, numbers and names, never a path to load */
    "MW_AROAD_DROP", "MW_DOMAIN", "MW_DSCP", "MW_DSCP_CARRIER", "MW_GAMESCOPE_APP",
    "MW_GAMESCOPE_BIN", "MW_GAMESCOPE_LINGER_S", "MW_LAN_ONLY", "MW_LATENCY_BEEP",
    "MW_LATENCY_FLAG_SKIP", "MW_LATENCY_FLAG_SOUND", "MW_LATENCY_FLAG_TRACE",
    "MW_MEDIA_QUEUED_VIDEO", "MW_NO_TELEMETRY", "MW_PORTAL_RESTORE_TOKEN", "MW_RTP_VIDEO",
    "MW_SCALER", "MW_SCTP_RTO_MIN_MS", "MW_SCTP_SACK_DELAY_MS", "MW_SERVICE", "MW_SHARED_FEED",
    "MW_VK_CONVERT_FAIL_AT", "MW_VK_ENCODE_DEPTH"};

/* Kept by prefix: the locale's categories and the login session's. */
static const char* const mw_launch_env_prefixes[] = {"LC_", "XDG_SESSION_"};

/* Set whatever the caller had. */
static const char* const mw_launch_env_fixed[] = {
    "HOME=/nonexistent",
    "XDG_CONFIG_HOME=/nonexistent",
    "XDG_DATA_HOME=/nonexistent",
    "XDG_CACHE_HOME=/nonexistent",
    "XDG_STATE_HOME=/nonexistent",
    "XDG_CONFIG_DIRS=/etc/xdg",
    "XDG_DATA_DIRS=/usr/local/share:/usr/share",
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin:"
    "/var/lib/flatpak/exports/bin"};

#define MW_LAUNCH_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Room mw_launch_env needs on top of the caller's entries: the fixed ones,
 * XAUTHORITY, MW_USER_HOME, the terminating NULL. */
#define MW_LAUNCH_ENV_EXTRA ((int)MW_LAUNCH_COUNT(mw_launch_env_fixed) + 3)

/* Whether the entry NAME=value names @p name. */
static inline int mw_launch_env_is(const char* entry, const char* name)
{
    const size_t n = strlen(name);
    return strncmp(entry, name, n) == 0 && entry[n] == '=';
}

/* The locale's and the time zone's values name what the C library reads, by
 * name under its own directories — and a value that is a path would make it
 * read from elsewhere. Kept as names only: no '/' in a locale, none leading a
 * time zone (Europe/Paris is a name; :/etc/x and /etc/x are paths), no "..". */
static inline int mw_launch_env_value_ok(const char* entry)
{
    const char* value = strchr(entry, '=') + 1;
    if (mw_launch_env_is(entry, "TZ")) {
        if (*value == ':') ++value;
        return *value != '/' && !strstr(value, "..");
    }
    if (mw_launch_env_is(entry, "LANG") || mw_launch_env_is(entry, "LANGUAGE") ||
        strncmp(entry, "LC_", 3) == 0)
        return !strchr(value, '/') && !strstr(value, "..");
    return 1;
}

static inline int mw_launch_env_kept(const char* entry)
{
    size_t i;
    if (!strchr(entry, '=')) return 0;
    for (i = 0; i < MW_LAUNCH_COUNT(mw_launch_env_names); ++i)
        if (mw_launch_env_is(entry, mw_launch_env_names[i])) return mw_launch_env_value_ok(entry);
    for (i = 0; i < MW_LAUNCH_COUNT(mw_launch_env_prefixes); ++i) {
        const size_t n = strlen(mw_launch_env_prefixes[i]);
        if (strncmp(entry, mw_launch_env_prefixes[i], n) == 0 && entry[n] != '=')
            return mw_launch_env_value_ok(entry);
    }
    return 0;
}

/* "<prefix><home><suffix>" into @p buffer; 0 when it does not fit. */
static inline int mw_launch_env_join(char* buffer, size_t size, const char* prefix,
                                     const char* home, const char* suffix)
{
    const size_t a = strlen(prefix), b = strlen(home), c = strlen(suffix);
    if (!buffer || a + b + c + 1 > size) return 0;
    memcpy(buffer, prefix, a);
    memcpy(buffer + a, home, b);
    memcpy(buffer + a + b, suffix, c + 1);
    return 1;
}

/* The environment of a mode handed a capability, into @p out (NULL-terminated,
 * @p cap slots): the kept entries of @p in (pointers into it), then the fixed
 * ones, then two derived from the caller's home when it is an absolute path:
 *  - MW_USER_HOME=<home>: where the app's own code finds what it reads there as
 *    data — the user's Steam library, its cache of what the encoders proved —
 *    under a name no library looks at. Never taken from the caller as is.
 *  - XAUTHORITY=<home>/.Xauthority when the caller had no XAUTHORITY: X looks
 *    for its cookie in the home folder otherwise, and that one is fixed now.
 * Written into @p home_buf and @p xauth (each @p buf_size bytes). Returns the
 * number of entries, or -1 when @p out or a buffer is too small. */
static inline int mw_launch_env(char* const* in, const char** out, int cap, char* home_buf,
                                char* xauth, size_t buf_size)
{
    int n = 0;
    int have_xauth = 0;
    const char* home = NULL;
    size_t i;
    for (; in && *in; ++in) {
        if (mw_launch_env_is(*in, "HOME") && !home) home = *in + 5;
        if (!mw_launch_env_kept(*in)) continue;
        if (mw_launch_env_is(*in, "XAUTHORITY")) have_xauth = 1;
        if (n >= cap - 1) return -1;
        out[n++] = *in;
    }
    for (i = 0; i < MW_LAUNCH_COUNT(mw_launch_env_fixed); ++i) {
        if (n >= cap - 1) return -1;
        out[n++] = mw_launch_env_fixed[i];
    }
    if (home && home[0] == '/') {
        if (n >= cap - 1 || !mw_launch_env_join(home_buf, buf_size, "MW_USER_HOME=", home, ""))
            return -1;
        out[n++] = home_buf;
        if (!have_xauth) {
            if (n >= cap - 1 ||
                !mw_launch_env_join(xauth, buf_size, "XAUTHORITY=", home, "/.Xauthority"))
                return -1;
            out[n++] = xauth;
        }
    }
    out[n] = NULL;
    return n;
}

#endif /* MOONLIGHTWEB_LAUNCH_ENV_H */
