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

#include "NativeHostBackend.h"

#include "../../common/Logger.h"

extern "C" {
#include "Limelight.h"
}

#include "mw/native/NativeHost.h"

#include <QHostInfo>

namespace {

/// One probe per call, deliberately. A display can be unplugged and a user can
/// log out between two requests, and a cached "available" would offer a host
/// that cannot start. The engine's probe is built to be cheap for this reason.
mw::native::Capabilities probeEngine()
{
    return mw::native::NativeHost::probe();
}

/// Displays are presented to the browser as apps, so an app id has to survive
/// the round trip and come back meaning the same display. Display ids start at
/// 0 and NvApp treats id 0 as uninitialized, so the wire id is offset by one.
constexpr int kAppIdBase = 1;

int displayIdToAppId(int displayId)
{
    return displayId + kAppIdBase;
}

int appIdToDisplayId(int appId)
{
    return appId - kAppIdBase;
}

} // namespace

bool NativeHostBackend::isAvailable()
{
    return probeEngine().available;
}

QString NativeHostBackend::unavailableReason()
{
    const mw::native::Capabilities caps = probeEngine();
    if (caps.available) return QString();
    return caps.diagnostic.empty() ? QString::fromUtf8(mw::native::toString(caps.reason))
                                   : QString::fromStdString(caps.diagnostic);
}

int NativeHostBackend::codecModeSupport()
{
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) return 0;

    // The union across GPUs. Which one a session actually gets is decided per
    // display by the engine's own Selector; this answers the broader question
    // the host list asks — "can this machine encode X at all".
    bool h264 = false, hevc = false, av1 = false, tenBit = false, yuv444 = false;
    for (const mw::native::GpuInfo& gpu : caps.gpus) {
        if (gpu.codecs.empty()) continue; // an API with no codec is no encoder
        for (mw::native::Codec codec : gpu.codecs) {
            switch (codec) {
            case mw::native::Codec::H264: h264 = true; break;
            case mw::native::Codec::Hevc: hevc = true; break;
            case mw::native::Codec::Av1: av1 = true; break;
            }
        }
        if (gpu.supports10Bit) tenBit = true;
        if (gpu.supports444) yuv444 = true;
    }

    int mask = 0;
    if (h264) mask |= SCM_H264;
    if (hevc) mask |= SCM_HEVC;
    if (av1) mask |= SCM_AV1_MAIN8;
    if (tenBit) {
        if (hevc) mask |= SCM_HEVC_MAIN10;
        if (av1) mask |= SCM_AV1_MAIN10;
    }
    if (yuv444) {
        if (h264) mask |= SCM_H264_HIGH8_444;
        if (hevc) mask |= SCM_HEVC_REXT8_444;
        if (hevc && tenBit) mask |= SCM_HEVC_REXT10_444;
        if (av1) mask |= SCM_AV1_HIGH8_444;
        if (av1 && tenBit) mask |= SCM_AV1_HIGH10_444;
    }
    return mask;
}

QString NativeHostBackend::hostDisplayName()
{
    QString host = QHostInfo::localHostName();
    if (host.isEmpty()) host = QStringLiteral("This PC");
    // The suffix is what makes the card unmistakable in a list that may also
    // hold Sunshine and Wolf hosts.
    return QStringLiteral("%1 — MoonlightWeb Host").arg(host);
}

BackendCapabilities NativeHostBackend::capabilities() const
{
    BackendCapabilities caps;
    // One physical screen cannot be handed to two players independently, so
    // multiUser stays false until a virtual display exists; provisioning and
    // lobbies have no meaning here at all.
    //
    // What IS true, and true only here: several displays stream at once. Each
    // has its own duplication and its own encoder session, so a second stream
    // takes nothing away from the first — unlike a GameStream host, which runs
    // one app at a time and would quit the first to start the second.
    caps.concurrentApps = true;
    return caps;
}

void NativeHostBackend::ensurePaired(BackendVoidCallback cb)
{
    // No PIN, no certificate, no association. There are not two parties here:
    // MoonlightWeb would be authenticating to itself.
    if (cb) cb(true, BackendError{});
}

void NativeHostBackend::listSeats(BackendSeatListCallback cb)
{
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           {});
        return;
    }

    SeatRef seat;
    seat.id = seatId();
    seat.name = hostDisplayName();
    seat.busy = false;
    cb(true, BackendError{}, {seat});
}

void NativeHostBackend::allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb)
{
    Q_UNUSED(deviceSessionId);
    if (!cb) return;

    // There is exactly one seat and it is never owned: the machine's screen is
    // shared by whoever is streaming it, which is the same thing a single
    // Sunshine host does today.
    SeatRef seat;
    seat.id = seatId();
    seat.name = hostDisplayName();
    cb(true, BackendError{}, seat);
}

void NativeHostBackend::releaseSeat(const QString& seatId)
{
    Q_UNUSED(seatId);
}

void NativeHostBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    Q_UNUSED(seatId);
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           {});
        return;
    }

    // One app per display. The existing app grid then IS the display picker
    // asked for in the plan — no new screen to build, and a single-monitor
    // machine shows one card that streams on one click.
    QVector<NvApp> apps;
    apps.reserve(caps.displays.size());
    for (const mw::native::DisplayInfo& display : caps.displays) {
        const bool hdrCapable = display.hdrActive && [&] {
            const mw::native::GpuInfo* gpu = caps.gpuFor(display);
            return gpu && gpu->supports10Bit;
        }();
        NvApp app(displayIdToAppId(display.id), QString::fromStdString(display.label), hdrCapable);
        // A monitor has no cover art, and there is no library here to look it up
        // in. Saying so spares the grid one 404 per display, on every render.
        app.setHasBoxArt(false);
        apps.append(app);
    }
    cb(true, BackendError{}, apps);
}

void NativeHostBackend::launch(const QString& seatId, const LaunchRequest& req,
                               BackendMediaCallback cb)
{
    Q_UNUSED(seatId);
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        const QString why = caps.diagnostic.empty()
                                ? QString::fromUtf8(mw::native::toString(caps.reason))
                                : QString::fromStdString(caps.diagnostic);
        Logger::warning(QStringLiteral("NativeHostBackend: launch refused — %1").arg(why));
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           MediaDescriptor{});
        return;
    }

    const int displayId = appIdToDisplayId(req.appId);
    bool known = false;
    for (const mw::native::DisplayInfo& display : caps.displays) {
        if (display.id == displayId) {
            known = true;
            break;
        }
    }
    if (!known) {
        // Almost always a display unplugged since the app list was fetched.
        cb(false,
           BackendError::make(BackendError::NotFound,
                              QStringLiteral("That display is no longer connected")),
           MediaDescriptor{});
        return;
    }

    // No network call, no XML, no key exchange — just the choice, handed to the
    // engine by the session that is about to build it.
    MediaDescriptor media;
    media.type = MediaType::NativeHost;
    media.nativeHost.displayId = displayId;
    media.nativeHost.hdrRequested = req.hdrEnabled;
    media.nativeHost.rideOutLoss = req.rideOutLoss;
    cb(true, BackendError{}, media);
}

void NativeHostBackend::resume(const QString& seatId, const LaunchRequest& req,
                               BackendMediaCallback cb)
{
    // Nothing survives a stream here: there is no host-side session to rejoin,
    // so resuming is starting. Keeping them identical means the launch/resume
    // self-heal in StreamSession stays correct without a special case.
    launch(seatId, req, std::move(cb));
}

void NativeHostBackend::quit(const QString& seatId, const QString& clientUniqueId,
                             BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    Q_UNUSED(clientUniqueId);
    // The engine is owned by the session, and stopping it is the session's own
    // teardown. There is no host-side state left to reclaim.
    if (cb) cb(true, BackendError{});
}

void NativeHostBackend::provisionSeat(const QJsonObject& params, BackendSeatCallback cb)
{
    Q_UNUSED(params);
    if (cb)
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has a single, permanent seat")),
           SeatRef{});
}

void NativeHostBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    if (cb)
        cb(false, BackendError::make(BackendError::Unsupported,
                                     QStringLiteral("This backend has a single, permanent seat")));
}
