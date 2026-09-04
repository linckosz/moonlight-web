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

    /// Whether the native engine can run on this machine right now. Cheap; safe
    /// to call from a poll. Used to decide whether the host card exists at all.
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

    /// "<hostname> — MoonlightWeb Host".
    static QString hostDisplayName();

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
