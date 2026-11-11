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

#pragma once

#include "IStreamBackend.h"

#include <QJsonObject>
#include <QString>

/**
 * @brief This machine, streamed by MoonlightWeb's own capture engine.
 *
 * The provider behind "<hostname> — MoonlightWeb Host". It is the thinnest
 * backend of the four, and every place it does less than the others is a
 * deliberate consequence of the engine living inside this process:
 *
 *  - **No pairing.** ensurePaired() answers yes immediately. There is no PIN,
 *    no certificate and no association step, because there are no two parties
 *    to authenticate — MoonlightWeb is asking itself.
 *  - **No network.** launch() dials nothing and negotiates nothing; it returns
 *    a MediaDescriptor naming a display. Everything a remote host needs (RTSP,
 *    RTP, FEC, a second layer of AES on top of DTLS) is precisely what the
 *    native engine exists to remove.
 *  - **Displays are the app list.** getAppList() returns one entry per display
 *    ("Display 1"), so the existing app grid IS the display picker. No new
 *    screen, and with a single monitor it is one card and one click.
 *
 * Availability is never assumed: every call re-asks the engine, because a
 * display can be unplugged and a user can log out between two requests. When
 * the engine cannot run here the backend fails with a plain message and the
 * host simply does not appear — which is what lets the Hosts page offer
 * Sunshine instead.
 *
 * "The engine" may live in another session: as a Windows service (session 0)
 * the probe and the stream both run in the console session as the logged-on
 * user (ConsoleSession.h), and what this backend re-asks is the latest probe
 * snapshot (NativeProbeService). The rules above hold unchanged; only where
 * the answer comes from differs.
 */
class NativeHostBackend : public IStreamBackend
{
public:
    /// The backend type name, as registered and as stored on NvComputer.
    static QString typeName() { return QStringLiteral("native"); }

    /// The uuid the synthetic native host carries in the host list.
    ///
    /// Stable across restarts and distinct from any real host's: a GameStream
    /// host's uuid comes from its own serverinfo, so a fixed literal here can
    /// never collide, and stability is what lets the browser remember which
    /// host it was streaming.
    static QString hostUuid() { return QStringLiteral("moonlightweb-native-host"); }

    /// The single seat this backend has: the machine itself.
    static QString seatId() { return QStringLiteral("self"); }

    /// Whether the owner of this machine wants it offered as a host at all —
    /// settings.json "native_host_enabled", default true. A false here is not a
    /// capability verdict: the engine is installed and would work, it is simply
    /// not put in anyone's list (someone using MoonlightWeb as a front end for
    /// other hosts). Checked before the probe on purpose, so a machine that has
    /// opted out never spawns one.
    static bool isEnabled();

    /// Whether the native engine can run on this machine right now — enabled by
    /// the owner AND capable. Cheap; safe to call from a poll. Used to decide
    /// whether the host card exists at all.
    static bool isAvailable();

    /// Why it cannot run, in English, for logs — never shown to a user, who
    /// gets the single fallback sentence instead. Empty when available.
    static QString unavailableReason();

    /// What the native engine can encode, expressed as GameStream's own
    /// ServerCodecModeSupport bits (SCM_*).
    ///
    /// The host list, the codec auto-selection and the transport filter all
    /// read `NvComputer::serverCodecModeSupport`, and they read it for every
    /// host. Leaving it zero on the native host made that logic conclude the
    /// host "supports NO video codec" — it happened to stream anyway, on a
    /// path that never consulted the answer, which is exactly the kind of luck
    /// that stops holding.
    ///
    /// Zero when the engine is unavailable, which is then the truth.
    static int codecModeSupport();

    /// Whether this machine can code a session in PyroWave, the intra-only
    /// wavelet codec of a wired LAN (docs/design/ultra-lan-poc.md §6.43): a
    /// Windows host whose GPU has a D3D12 route (NVENC, AMF or oneVPL), where
    /// its encoder runs. Never Linux or macOS. Announced on the host's card
    /// (nativeDisplay.pyrowave), so a browser's Settings can offer the codec
    /// before any stream.
    static bool pyrowaveSupported();

    /// "<hostname> — MoonlightWeb Host".
    static QString hostDisplayName();

    /// The app id of "MoonlightWeb Virtual Display" — the one card that is
    /// there whether or not its display exists at the moment. A /start on it
    /// turns the display on first (VirtualDisplayJob); the last stream's end
    /// turns it off.
    static int virtualDisplayAppId();

    /// The app id of "This PC's screen", the one card of a session Remote
    /// Desktop left (Windows, RemoteDesktopReturn). A /start on it brings the
    /// session back to the PC's screen, locked, and then streams the display
    /// primaryDisplayAppId() names.
    static int remoteDesktopReturnAppId();
    /// "This PC's sign-in screen", the card of a session Remote Desktop left
    /// when this version offers it (RemoteDesktopReturnPolicy::kSignInAppId):
    /// a /start on it streams the console's sign-in screen.
    static int signInScreenAppId();

    /// The card id of the display a session brought back to the PC's screen is
    /// streamed on: the primary one, else the first real one. -1 with @p error
    /// while there is none (yet).
    static int primaryDisplayAppId(QString* error);

    /// Whether @p gpu may be chosen to draw the virtual display: empty (the
    /// driver's own choice), or the name of a GPU this machine has now, on a
    /// machine with several, while the driver's settings file is ours to write
    /// — the same test that puts the choice on the display's card.
    static bool virtualDisplayGpuChoosable(const QString& gpu);

    /// The display an app id stands for, on this machine now, by its
    /// DisplayInfo::key — or empty with @p error saying why (the virtual
    /// display is off, the display was unplugged, native streaming is
    /// unavailable). The server answers it for a worker — a stream's, for a
    /// display card (LaunchRequest::displayKey), and the guests' shared feed,
    /// which shows the owner's display: the worker, SYSTEM's maybe, does not
    /// read the settings file the display cards are numbered in.
    static QString displayKeyForApp(int appId, QString* error);
    /// The display id of the display @p key names (displayKeyForApp), in the
    /// worker's own probe, or -1 with @p error.
    static int displayForKey(const QString& key, QString* error);
    /// Whether @p appId is a display's card — numbered in the settings
    /// (AppSettings::displayAppIds), unlike the virtual display's and the
    /// owner's apps'.
    static bool isDisplayCard(int appId);
    /// Whether @p appId is the card of one of the owner's apps
    /// (AppSettings::hostApps), whether or not the app is still there.
    static bool isHostAppCard(int appId);
    /// The display our virtual display took the primary role from, by its
    /// DisplayInfo::key, as turning it on recorded it in the settings
    /// (VirtualDisplayJob) — or empty. The server reads it for a worker
    /// (LaunchRequest::previousPrimary).
    static QString previousPrimary();

    /// Where the owner's apps (AppSettings::hostApps) open, the same for all
    /// of them on a given machine: in a gamescope of their own (Linux, when
    /// it is usable), else on "MoonlightWeb Virtual Display" (turned on at
    /// the client's size and made primary, as its own card does), else on
    /// this PC's primary display.
    enum class HostAppScreen
    {
        Gamescope,
        Virtual,
        Primary
    };
    static HostAppScreen hostAppScreen();
    /// "gamescope", "virtual" or "primary", for the card and the admin page.
    static QString toString(HostAppScreen screen);

    /// The owner's app whose card is @p appId (AppSettings::hostApp), or an
    /// empty object for a display's card.
    static QJsonObject hostAppFor(int appId);

    /// The command the server opens on the desktop once a stream of @p appId
    /// is up (HostAppLauncher), or empty: a display's card, or an app that
    /// runs in its own gamescope, which the session starts itself.
    static QString desktopCommandFor(int appId);

    /// Whether a stream of @p appId streams "MoonlightWeb Virtual Display":
    /// its own card, or an app's card when apps open there. Such a stream
    /// turns the display on at its /start and lets it go at its end.
    static bool usesVirtualDisplay(int appId);

    /// Whether @p appId is one of the owner's apps that runs in gamescope (a
    /// Linux native host where gamescope is usable), and which: its name and
    /// command, as the session runs them.
    static bool gamescopeAppFor(int appId, QString* name, QString* command);

    NativeHostBackend() = default;

    QString type() const override { return typeName(); }
    BackendCapabilities capabilities() const override;

    void ensurePaired(BackendVoidCallback cb) override;

    void listSeats(BackendSeatListCallback cb) override;
    void allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb) override;
    void releaseSeat(const QString& seatId) override;

    void getAppList(const QString& seatId, BackendAppListCallback cb) override;

    void launch(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb) override;
    void resume(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb) override;
    void quit(const QString& seatId, const QString& clientUniqueId,
              BackendVoidCallback cb) override;

    void provisionSeat(const QJsonObject& params, BackendSeatCallback cb) override;
    void teardownSeat(const QString& seatId, BackendVoidCallback cb) override;
};
