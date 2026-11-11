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

#include "../../backend/VirtualDisplay.h"
#include "../../common/Logger.h"
#include "../../server/AppSettings.h"
#include "../../streaming/RemoteDesktopReturn.h"

extern "C" {
#include "Limelight.h"
}

#include "HostOsProbe.h"
#include "NativeProbeService.h"

#include "NativeCapabilitiesJson.h"
#include "mw/native/NativeHost.h"

#include <QCollator>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QHostInfo>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <vector>

namespace {

/// One probe per call, deliberately. A display can be unplugged and a user can
/// log out between two requests, and a cached "available" would offer a host
/// that cannot start. The engine's probe is built to be cheap for this reason.
///
/// As a service the probe runs in another session and this is its latest
/// snapshot instead (see NativeProbeService) — asking still costs nothing, and
/// asking is what keeps the snapshot fresh.
mw::native::Capabilities probeEngine()
{
    return NativeProbeService::instance().snapshot();
}

/// "MoonlightWeb Virtual Display" is a card whether or not the display exists
/// right now — it is off between streams — so it cannot be numbered after a
/// display index. A fixed id far above any display's: a machine has a handful
/// of monitors, never a thousand.
constexpr int kVirtualDisplayAppId = 1000;
static_assert(AppSettings::kDisplayAppIdEnd <= kVirtualDisplayAppId,
              "display cards are numbered below the virtual display's");

/// Each display's DisplayInfo::key, in @p caps' order — told apart when two
/// screens share one (on macOS, two monitors of one model that report no
/// serial number): the second one seen is "<key>#2", and so on.
QStringList displayKeys(const mw::native::Capabilities& caps)
{
    QStringList keys;
    QHash<QString, int> seen;
    for (const mw::native::DisplayInfo& display : caps.displays) {
        QString key = QString::fromStdString(display.key);
        const int nth = ++seen[key.toLower()];
        if (nth > 1) key += QStringLiteral("#%1").arg(nth);
        keys.append(key);
    }
    return keys;
}

/// Displays are presented to the browser as apps, so an app id has to survive
/// the round trip and come back meaning the same display — not the same rank:
/// a display that appears or turns primary moves the others' (on Windows, our
/// virtual display goes first on its GPU). Each screen keeps an id of its own
/// (AppSettings::displayAppIds), first given the number its card had when ids
/// were ranks: its rank plus one, NvApp treating id 0 as uninitialized. 0 for
/// the entries with no card numbered here: ours, which has its fixed id, and
/// the one the owner's apps in gamescope share.
QList<int> displayAppIds(const mw::native::Capabilities& caps)
{
    QStringList keys = displayKeys(caps);
    QList<int> firstIds;
    for (size_t i = 0; i < caps.displays.size(); ++i) {
        const mw::native::DisplayInfo& display = caps.displays[i];
        if (display.key == mw::native::kGamescopeAppDisplayKey || VirtualDisplay::isOurs(display))
            keys[static_cast<int>(i)].clear();
        firstIds.append(display.id + 1);
    }
    return AppSettings().displayAppIds(keys, firstIds);
}

/// The owner's apps that ran in gamescope in 0.4.0's betas kept their card
/// ids, 1100 + their rank; every app since is numbered from
/// AppSettings::kHostAppIdFirst. Below 1100: displays and the virtual display.
constexpr int kHostAppIdFloor = 1100;
static_assert(RemoteDesktopReturnPolicy::kReturnAppId > kVirtualDisplayAppId &&
                  RemoteDesktopReturnPolicy::kReturnAppId < kHostAppIdFloor,
              "\"This PC's screen\" sits between the virtual display and the owner's apps");

/// The engine's entry for the owner's apps in gamescope, or nullptr.
const mw::native::DisplayInfo* gamescopeAppsDisplay(const mw::native::Capabilities& caps)
{
    for (const mw::native::DisplayInfo& display : caps.displays)
        if (display.key == mw::native::kGamescopeAppDisplayKey) return &display;
    return nullptr;
}

/// Whether gamescope's own Steam Big Picture card is listed (Linux): a Steam
/// card seeded from the settings would then be a second one.
bool gamescopeSteamListed(const mw::native::Capabilities& caps)
{
    for (const mw::native::DisplayInfo& display : caps.displays)
        if (display.key == mw::native::kGamescopeSteamDisplayKey) return true;
    return false;
}

NativeHostBackend::HostAppScreen screenOf(const mw::native::Capabilities& caps)
{
    if (gamescopeAppsDisplay(caps)) return NativeHostBackend::HostAppScreen::Gamescope;
    if (VirtualDisplay::probe().installed) return NativeHostBackend::HostAppScreen::Virtual;
    return NativeHostBackend::HostAppScreen::Primary;
}

/// The display a program opened on "this PC's screen" lands on: the primary
/// one, else the first real one, or nullptr.
const mw::native::DisplayInfo* primaryDisplay(const mw::native::Capabilities& caps)
{
    const mw::native::DisplayInfo* first = nullptr;
    for (const mw::native::DisplayInfo& display : caps.displays) {
        if (mw::native::isMadeForStreamKey(display.key) || VirtualDisplay::isOurs(display))
            continue;
        if (display.primary) return &display;
        if (!first) first = &display;
    }
    return first;
}

/// The display @p appId stands for among @p caps, or -1 with @p why. For one
/// of the owner's apps, @p hostAppKnown says whether it is still in the
/// settings when the server already found out (LaunchRequest::hostAppKnown).
int displayOf(const mw::native::Capabilities& caps, int appId, QString* why,
              std::optional<bool> hostAppKnown = std::nullopt)
{
    const auto virtualDisplay = [&caps, why]() {
        // Our virtual display, turned on by the server before this launch
        // (main.cpp's /start): whichever display index it came up under.
        for (const mw::native::DisplayInfo& display : caps.displays)
            if (VirtualDisplay::isOurs(display)) return display.id;
        if (why) *why = QStringLiteral("The virtual display is not on");
        return -1;
    };
    if (appId == kVirtualDisplayAppId) return virtualDisplay();
    if (appId >= kHostAppIdFloor) {
        if (!hostAppKnown.value_or(!AppSettings().hostApp(appId).isEmpty())) {
            if (why) *why = QStringLiteral("That app is no longer in the host's settings");
            return -1;
        }
        switch (screenOf(caps)) {
        case NativeHostBackend::HostAppScreen::Gamescope: return gamescopeAppsDisplay(caps)->id;
        case NativeHostBackend::HostAppScreen::Virtual: return virtualDisplay();
        case NativeHostBackend::HostAppScreen::Primary:
            if (const mw::native::DisplayInfo* primary = primaryDisplay(caps)) return primary->id;
            if (why) *why = QStringLiteral("This PC has no screen to open the app on");
            return -1;
        }
    }
    if (appId > 0 && appId < AppSettings::kDisplayAppIdEnd) {
        const QList<int> ids = displayAppIds(caps);
        for (int i = 0; i < ids.size(); ++i)
            if (ids.at(i) == appId) return caps.displays[static_cast<size_t>(i)].id;
    }
    // Almost always a display unplugged since the app list was fetched.
    if (why) *why = QStringLiteral("That display is no longer connected");
    return -1;
}

/// The display @p key names (displayKeys) among @p caps, or -1 with @p why.
int displayWithKey(const mw::native::Capabilities& caps, const QString& key, QString* why)
{
    const QStringList keys = displayKeys(caps);
    if (!key.isEmpty())
        for (size_t i = 0; i < caps.displays.size(); ++i)
            if (keys.at(static_cast<int>(i)).compare(key, Qt::CaseInsensitive) == 0)
                return caps.displays[i].id;
    if (why) *why = QStringLiteral("That display is no longer connected");
    return -1;
}

/// The GPU that draws our virtual display (@p displayId) and the one that drew
/// the display it took the primary role from, when they are not the same GPU;
/// both left empty otherwise. Apps started before the stream render on the
/// second (see NativeHostMedia::virtualDisplayGpu). That display is
/// @p previousPrimary when the server said which (LaunchRequest).
void gpusApart(const mw::native::Capabilities& caps, int displayId,
               const std::optional<QString>& previousPrimary, QString* virtualGpu,
               QString* previousGpu)
{
    const QString previous =
        previousPrimary ? *previousPrimary : NativeHostBackend::previousPrimary();
    if (previous.isEmpty()) return;
    int oursGpu = -1;
    int theirsGpu = -1;
    for (const mw::native::DisplayInfo& display : caps.displays) {
        if (display.id == displayId)
            oursGpu = display.gpuId;
        else if (QString::fromStdString(display.key).compare(previous, Qt::CaseInsensitive) == 0)
            theirsGpu = display.gpuId;
    }
    if (oursGpu < 0 || theirsGpu < 0 || oursGpu == theirsGpu) return;
    const auto nameOf = [&caps](int gpuId) {
        for (const mw::native::GpuInfo& gpu : caps.gpus)
            if (gpu.id == gpuId) return QString::fromStdString(gpu.name);
        return QString();
    };
    *virtualGpu = nameOf(oursGpu);
    *previousGpu = nameOf(theirsGpu);
    if (virtualGpu->isEmpty() || previousGpu->isEmpty()) {
        virtualGpu->clear();
        previousGpu->clear();
    }
}

/// @p device with what displayGpu says of the GPU behind it, if anything.
void addGpu(QJsonObject& device, const QJsonObject& gpu)
{
    for (auto it = gpu.begin(); it != gpu.end(); ++it)
        device.insert(it.key(), it.value());
}

/// The virtual display driver's settings file as it stands: whether it is
/// ours to write (none yet counts: the next activation writes ours), and the
/// GPU it names. Not ours — an owner's own VDD, whose file draws that owner's
/// displays too — and the card only says which GPU that file names.
struct DriverSettings
{
    bool ours = false;
    QString gpu;
};

DriverSettings driverSettings()
{
#ifdef Q_OS_WIN
    QFile f(VirtualDisplay::settingsXmlPath());
    if (!f.exists()) return {true, QString()};
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString xml = QString::fromUtf8(f.readAll());
    if (xml.trimmed().isEmpty()) return {true, QString()};
    return {VirtualDisplay::isOurSettings(xml), VirtualDisplay::gpuOfSettings(xml)};
#else
    // Only the Windows driver is told which GPU to draw on.
    return {};
#endif
}

/// The virtual display's card. @p on is the display while it is on, which
/// says the GPU that draws it; off, the card names the GPU it will be drawn by
/// when one is set — and, where the settings file is ours, offers the choice
/// (device.gpuChoice: the owner's pick, "" for the driver's own, and the GPUs
/// by name — the driver tells two GPUs apart by nothing else).
NvApp virtualDisplayApp(const mw::native::Capabilities& caps,
                        const mw::native::DisplayInfo* on = nullptr)
{
    NvApp app(kVirtualDisplayAppId, VirtualDisplay::displayName(), /*hdr=*/false);
    app.setHasBoxArt(false);
    QJsonObject device{
        {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
        {QStringLiteral("display"), QStringLiteral("virtual")},
        {QStringLiteral("model"), VirtualDisplay::displayName()},
        {QStringLiteral("key"), QStringLiteral("moonlightweb-virtual-display")},
        {QStringLiteral("battery"), caps.hasBattery},
        // What the card says about itself: MoonlightWeb's own display, not a
        // monitor somebody plugged in.
        {QStringLiteral("owned"), true},
    };
    if (caps.gpus.size() > 1) {
        const DriverSettings settings = driverSettings();
        const QString chosen = settings.ours ? AppSettings().virtualDisplayGpu() : QString();
        if (on) {
            addGpu(device, NativeCapabilitiesJson::displayGpu(caps, on->gpuId));
        } else {
            const QString named = settings.ours ? chosen : settings.gpu;
            if (!named.isEmpty()) device[QStringLiteral("gpu")] = named;
        }
        if (settings.ours) {
            QJsonArray names;
            for (const mw::native::GpuInfo& gpu : caps.gpus) {
                const QString name = QString::fromStdString(gpu.name);
                if (VirtualDisplay::validGpuName(name) && !names.contains(name)) names.append(name);
            }
            device[QStringLiteral("gpuChoice")] = QJsonObject{
                {QStringLiteral("selected"), chosen},
                {QStringLiteral("options"), names},
            };
        }
    }
    app.setDevice(device);
    return app;
}

/// "This PC's screen": the card of a session Remote Desktop left without a
/// screen. It draws the PC with its monitor, the screen it brings the session
/// back to; the browser names it in its own language.
NvApp remoteDesktopReturnApp(const mw::native::Capabilities& caps)
{
    NvApp app(RemoteDesktopReturnPolicy::kReturnAppId, QStringLiteral("This PC's screen (locked)"),
              /*hdr=*/false);
    app.setHasBoxArt(false);
    app.setDevice(QJsonObject{
        {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
        {QStringLiteral("display"),
         caps.hasBattery ? QStringLiteral("builtin") : QStringLiteral("external")},
        {QStringLiteral("model"), QString()},
        {QStringLiteral("key"), QStringLiteral("remote-desktop-return")},
        {QStringLiteral("battery"), caps.hasBattery},
    });
    return app;
}

/// "This PC's sign-in screen": the one card of a session Remote Desktop left,
/// when this version offers it (RemoteDesktopReturnPolicy::kSignInOffered).
/// Opening it streams the console's sign-in screen, where the user signs in
/// with their own account and Windows brings their session back.
NvApp signInScreenApp(const mw::native::Capabilities& caps)
{
    NvApp app(RemoteDesktopReturnPolicy::kSignInAppId, QStringLiteral("This PC's sign-in screen"),
              /*hdr=*/false);
    app.setHasBoxArt(false);
    app.setDevice(QJsonObject{
        {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
        {QStringLiteral("display"),
         caps.hasBattery ? QStringLiteral("builtin") : QStringLiteral("external")},
        {QStringLiteral("model"), QString()},
        {QStringLiteral("key"), QStringLiteral("console-sign-in")},
        {QStringLiteral("battery"), caps.hasBattery},
    });
    return app;
}

/// One card per app of the owner's (AppSettings::hostApps), in the settings'
/// order, after @p apps' displays. The card says where its program opens
/// (device.screen) and, when the owner gave it one, its image's hash
/// (device.art), which changes the card's signature when the image does.
void appendHostApps(const mw::native::Capabilities& caps, QVector<NvApp>& apps)
{
    const NativeHostBackend::HostAppScreen screen = screenOf(caps);
    const mw::native::DisplayInfo* primary =
        screen == NativeHostBackend::HostAppScreen::Primary ? primaryDisplay(caps) : nullptr;
    if (screen == NativeHostBackend::HostAppScreen::Primary && !primary) return;
    const bool hideSteam = gamescopeSteamListed(caps);
    for (const QJsonValue& v : AppSettings().hostApps()) {
        const QJsonObject entry = v.toObject();
        if (hideSteam && entry.value("source").toString() == QLatin1String("steam")) continue;
        const QString name = entry.value("name").toString();
        const QString id = entry.value("id").toString();
        const QString image = entry.value("image").toString();
        NvApp app(entry.value("app_id").toInt(), name, /*hdr=*/false);
        app.setHasBoxArt(!image.isEmpty());
        QJsonObject device{
            {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
            {QStringLiteral("display"),
             primary ? QString::fromLatin1(mw::native::toString(primary->kind))
                     : QStringLiteral("virtual")},
            {QStringLiteral("model"), name},
            {QStringLiteral("key"), (screen == NativeHostBackend::HostAppScreen::Gamescope
                                         ? QStringLiteral("gamescope-app-")
                                         : QStringLiteral("host-app-")) +
                                        id},
            {QStringLiteral("battery"), caps.hasBattery},
            {QStringLiteral("screen"), NativeHostBackend::toString(screen)},
        };
        if (!image.isEmpty()) device[QStringLiteral("art")] = image;
        app.setDevice(device);
        apps.append(app);
    }
}

} // namespace

int NativeHostBackend::virtualDisplayAppId()
{
    return kVirtualDisplayAppId;
}

int NativeHostBackend::remoteDesktopReturnAppId()
{
    return RemoteDesktopReturnPolicy::kReturnAppId;
}

int NativeHostBackend::signInScreenAppId()
{
    return RemoteDesktopReturnPolicy::kSignInAppId;
}

int NativeHostBackend::primaryDisplayAppId(QString* error)
{
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        if (error)
            *error = caps.diagnostic.empty() ? QString::fromUtf8(mw::native::toString(caps.reason))
                                             : QString::fromStdString(caps.diagnostic);
        return -1;
    }
    const mw::native::DisplayInfo* primary = primaryDisplay(caps);
    if (!primary) {
        if (error) *error = QStringLiteral("This PC has no screen to stream");
        return -1;
    }
    const QList<int> ids = displayAppIds(caps);
    const int appId = ids.value(static_cast<int>(primary - caps.displays.data()));
    if (appId <= 0) {
        if (error) *error = QStringLiteral("This PC's screen has no card");
        return -1;
    }
    return appId;
}

bool NativeHostBackend::virtualDisplayGpuChoosable(const QString& gpu)
{
    if (!VirtualDisplay::probe().installed || !driverSettings().ours) return false;
    if (gpu.isEmpty()) return true;
    const mw::native::Capabilities caps = probeEngine();
    if (caps.gpus.size() < 2 || !VirtualDisplay::validGpuName(gpu)) return false;
    for (const mw::native::GpuInfo& known : caps.gpus)
        if (QString::fromStdString(known.name) == gpu) return true;
    return false;
}

bool NativeHostBackend::gamescopeAppFor(int appId, QString* name, QString* command)
{
    if (appId < kHostAppIdFloor || hostAppScreen() != HostAppScreen::Gamescope) return false;
    const QJsonObject app = AppSettings().hostApp(appId);
    if (app.isEmpty()) return false;
    if (name) *name = app.value("name").toString();
    if (command) *command = app.value("command").toString();
    return true;
}

QJsonObject NativeHostBackend::hostAppFor(int appId)
{
    if (appId < kHostAppIdFloor) return {};
    return AppSettings().hostApp(appId);
}

QString NativeHostBackend::desktopCommandFor(int appId)
{
    const QJsonObject app = hostAppFor(appId);
    if (app.isEmpty() || hostAppScreen() == HostAppScreen::Gamescope) return QString();
    return app.value("command").toString();
}

NativeHostBackend::HostAppScreen NativeHostBackend::hostAppScreen()
{
    return screenOf(probeEngine());
}

bool NativeHostBackend::usesVirtualDisplay(int appId)
{
    if (appId == kVirtualDisplayAppId) return true;
    // Whether the app is still in the settings does not matter: one removed
    // while it streams must still let the display go at the stream's end.
    return appId >= kHostAppIdFloor && hostAppScreen() == HostAppScreen::Virtual;
}

QString NativeHostBackend::toString(HostAppScreen screen)
{
    switch (screen) {
    case HostAppScreen::Gamescope: return QStringLiteral("gamescope");
    case HostAppScreen::Virtual: return QStringLiteral("virtual");
    case HostAppScreen::Primary: return QStringLiteral("primary");
    }
    return QString();
}

QString NativeHostBackend::displayKeyForApp(int appId, QString* error)
{
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        if (error)
            *error = caps.diagnostic.empty() ? QString::fromUtf8(mw::native::toString(caps.reason))
                                             : QString::fromStdString(caps.diagnostic);
        return QString();
    }
    const int displayId = displayOf(caps, appId, error);
    const QStringList keys = displayKeys(caps);
    for (size_t i = 0; i < caps.displays.size(); ++i)
        if (caps.displays[i].id == displayId) return keys.at(static_cast<int>(i));
    return QString();
}

int NativeHostBackend::displayForKey(const QString& key, QString* error)
{
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        if (error)
            *error = caps.diagnostic.empty() ? QString::fromUtf8(mw::native::toString(caps.reason))
                                             : QString::fromStdString(caps.diagnostic);
        return -1;
    }
    return displayWithKey(caps, key, error);
}

bool NativeHostBackend::isDisplayCard(int appId)
{
    return appId > 0 && appId < AppSettings::kDisplayAppIdEnd;
}

bool NativeHostBackend::isHostAppCard(int appId)
{
    return appId >= kHostAppIdFloor;
}

QString NativeHostBackend::previousPrimary()
{
    return AppSettings().virtualDisplay().value(QLatin1String("previous_primary")).toString();
}

bool NativeHostBackend::isEnabled()
{
    return AppSettings().nativeHostEnabled();
}

bool NativeHostBackend::isAvailable()
{
    // The owner's answer comes first, and short-circuits: a machine that has
    // opted out must not pay for a probe — nor, as a service, spawn one in the
    // console session — to be told about an engine nobody will be offered.
    return isEnabled() && probeEngine().available;
}

QString NativeHostBackend::unavailableReason()
{
    if (!isEnabled()) return QStringLiteral("disabled by native_host_enabled in settings.json");
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
    bool h264 = false, hevc = false, av1 = false, tenBit = false;
    bool h264_444 = false, hevc444 = false, av1_444 = false;
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
        // 4:4:4 is per codec: NVENC has it on H.264 and HEVC but not AV1, and
        // advertising AV1 4:4:4 here would have the browser prefer a mode the
        // encoder then refuses.
        if (gpu.supports444(mw::native::Codec::H264)) h264_444 = true;
        if (gpu.supports444(mw::native::Codec::Hevc)) hevc444 = true;
        if (gpu.supports444(mw::native::Codec::Av1)) av1_444 = true;
    }

    int mask = 0;
    if (h264) mask |= SCM_H264;
    if (hevc) mask |= SCM_HEVC;
    if (av1) mask |= SCM_AV1_MAIN8;
    if (tenBit) {
        if (hevc) mask |= SCM_HEVC_MAIN10;
        if (av1) mask |= SCM_AV1_MAIN10;
    }
    if (h264_444) mask |= SCM_H264_HIGH8_444;
    if (hevc444) {
        mask |= SCM_HEVC_REXT8_444;
        if (tenBit) mask |= SCM_HEVC_REXT10_444;
    }
    if (av1_444) {
        mask |= SCM_AV1_HIGH8_444;
        if (tenBit) mask |= SCM_AV1_HIGH10_444;
    }
    return mask;
}

bool NativeHostBackend::pyrowaveSupported()
{
#if defined(Q_OS_WIN)
    // The same machines as the admin's D3D12 choice (SystemRoutes'
    // nativeVideoPipelineSupported): a Snapdragon's Media Foundation encoder
    // has no D3D12 route, nor has a machine with no GPU encoder at all.
    if (!isEnabled()) return false;
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) return false;
    for (const mw::native::GpuInfo& gpu : caps.gpus)
        for (mw::native::EncoderApi api : gpu.encoders)
            if (api == mw::native::EncoderApi::Nvenc || api == mw::native::EncoderApi::Amf ||
                api == mw::native::EncoderApi::Vpl)
                return true;
#endif
    return false;
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

namespace {

// The displays' cards as last listed with every display on (issue #39).
QMutex s_LitMutex;
QVector<NvApp> s_LitDisplays;

void appendDarkDisplays(const QVector<NvApp>& physical, QVector<NvApp>& apps)
{
#ifdef Q_OS_WIN
    // While a stream has the other displays off, Windows has detached them:
    // they leave the list. Their cards stay, from the last listing that had
    // them on, marked "off" so the grid draws them dark and opens nothing.
    const bool othersOff = QFile::exists(VirtualDisplay::othersOffPath());
    QMutexLocker lock(&s_LitMutex);
    if (!othersOff) {
        s_LitDisplays = physical;
        return;
    }
    QList<int> lit, listed;
    for (const NvApp& app : std::as_const(s_LitDisplays))
        lit.append(app.id());
    for (const NvApp& app : std::as_const(apps))
        listed.append(app.id());
    for (int id : VirtualDisplay::darkDisplays(lit, listed, othersOff)) {
        NvApp app = s_LitDisplays.at(lit.indexOf(id));
        QJsonObject device = app.device();
        device[QStringLiteral("off")] = true;
        app.setDevice(device);
        apps.append(app);
    }
#else
    Q_UNUSED(physical);
    Q_UNUSED(apps);
#endif
}

} // namespace

void NativeHostBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    Q_UNUSED(seatId);
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    // Our own virtual display is a card as long as it is installed, on and
    // off alike: opening it is what turns it on.
    const bool virtualInstalled = VirtualDisplay::probe().installed;
    if (!caps.available) {
        // Remote Desktop left this session with no screen, and may bring it
        // back: "This PC's screen" is the one card, and opening it is what
        // brings the session back (RemoteDesktopReturn).
        if (caps.reason == mw::native::Unavailability::NoDisplay &&
            RemoteDesktopReturn::instance().offersReturn()) {
            cb(true, BackendError{}, {remoteDesktopReturnApp(caps)});
            return;
        }
        // Or its sign-in screen is streamed instead, and the user signs in
        // through it: "This PC's sign-in screen" is then the one card.
        if (caps.reason == mw::native::Unavailability::NoDisplay &&
            RemoteDesktopReturn::instance().offersSignIn()) {
            cb(true, BackendError{}, {signInScreenApp(caps)});
            return;
        }
        // A machine with nothing attached and our display to offer has one
        // card; any other reason to be unavailable has none.
        // Not when Remote Desktop left this session: no display reaches a
        // disconnected session, ours included (bench of 09/11/2026).
        if (virtualInstalled && caps.reason == mw::native::Unavailability::NoDisplay &&
            !RemoteDesktopReturn::instance().leftByRemoteDesktop()) {
            // The owner's apps open on that same display.
            QVector<NvApp> apps;
            appendHostApps(caps, apps);
            apps.append(virtualDisplayApp(caps));
            cb(true, BackendError{}, apps);
            return;
        }
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           {});
        return;
    }

    // One app per display. The existing app grid then IS the display picker
    // asked for in the plan — no new screen to build, and a single-monitor
    // machine shows one card that streams on one click.
    //
    // The probe lists displays in the OS's enumeration order (on Windows, GPU
    // by GPU), which puts "Display 2" before "Display 1" as soon as the second
    // one hangs off the first adapter. The grid reads left to right, so the
    // primary comes first and the rest follow by label, numbers compared as
    // numbers ("Display 10" after "Display 2"). Only the order changes: each
    // app keeps the id of its own display.
    std::vector<const mw::native::DisplayInfo*> ordered;
    ordered.reserve(caps.displays.size());
    for (const mw::native::DisplayInfo& display : caps.displays)
        ordered.push_back(&display);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const mw::native::DisplayInfo* a, const mw::native::DisplayInfo* b) {
                         if (a->primary != b->primary) return a->primary;
                         return collator.compare(QString::fromStdString(a->label),
                                                 QString::fromStdString(b->label)) < 0;
                     });

    const QList<int> ids = displayAppIds(caps);
    QVector<NvApp> apps;
    apps.reserve(caps.displays.size() + 1);
    QVector<NvApp> physical; // the displays' own cards, not ours nor an app's
    bool virtualListed = false;
    for (const mw::native::DisplayInfo* entry : ordered) {
        const mw::native::DisplayInfo& display = *entry;
        // The owner's apps in gamescope have a card each, below — never one
        // for the entry they share.
        if (display.key == mw::native::kGamescopeAppDisplayKey) continue;
        if (VirtualDisplay::isOurs(display)) {
            // On right now (a stream is running on it): the same card as
            // when it is off, under its own name and its fixed id.
            virtualListed = true;
            apps.append(virtualDisplayApp(caps, &display));
            continue;
        }
        const bool hdrCapable = display.hdrActive && [&] {
            const mw::native::GpuInfo* gpu = caps.gpuFor(display);
            return gpu && gpu->supports10Bit;
        }();
        const int appId = ids.value(static_cast<int>(entry - caps.displays.data()));
        // A key the settings could not number (or not keep): no card rather
        // than one that opens another screen.
        if (appId <= 0) continue;
        NvApp app(appId, QString::fromStdString(display.label), hdrCapable);
        // A monitor has no cover art, and there is no library here to look it up
        // in. Saying so spares the grid one 404 per display, on every render.
        app.setHasBoxArt(false);
        // What the card draws instead: the machine this display belongs to.
        // The OS is the one this server was built for — the native host is
        // this machine by definition.
        QJsonObject device{
            {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
            {QStringLiteral("display"), QString::fromLatin1(mw::native::toString(display.kind))},
            {QStringLiteral("model"), QString::fromStdString(display.model)},
            {QStringLiteral("key"), QString::fromStdString(display.key)},
            {QStringLiteral("battery"), caps.hasBattery},
        };
        // And, on a PC with several GPUs, the one that draws this display.
        addGpu(device, NativeCapabilitiesJson::displayGpu(caps, display.gpuId));
        app.setDevice(device);
        apps.append(app);
        physical.append(app);
    }
    appendDarkDisplays(physical, apps);
    // The owner's apps (AppSettings::hostApps), in the settings' order.
    appendHostApps(caps, apps);
    if (virtualInstalled && !virtualListed) apps.append(virtualDisplayApp(caps));
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

    QString why;
    // A worker's display card comes resolved by the server, and an app card
    // known or not by it: this process may not read the settings the cards
    // are numbered in (LaunchRequest).
    //
    // The sign-in screen's worker streams the primary display of the session
    // it runs in, whatever the request names: the server, in the session
    // Remote Desktop left, sees no display at all to name.
    int displayId = -1;
    if (mw::native::NativeHost::signInScreenOnly()) {
        const mw::native::DisplayInfo* primary = primaryDisplay(caps);
        displayId = primary ? primary->id : -1;
        if (!primary) why = QStringLiteral("The sign-in screen has no display to stream");
    } else {
        displayId = req.displayKey ? displayWithKey(caps, *req.displayKey, &why)
                                   : displayOf(caps, req.appId, &why, req.hostAppKnown);
    }
    if (displayId < 0) {
        cb(false, BackendError::make(BackendError::NotFound, why), MediaDescriptor{});
        return;
    }

    // No network call, no XML, no key exchange — just the choice, handed to the
    // engine by the session that is about to build it.
    MediaDescriptor media;
    media.type = MediaType::NativeHost;
    media.nativeHost.displayId = displayId;
    media.nativeHost.hdrRequested = req.hdrEnabled;
    media.nativeHost.rideOutLoss = req.rideOutLoss;
    media.nativeHost.refInvalidation = req.refInvalidation;
    media.nativeHost.cropsToFrame = req.cropsToFrame;
    if (usesVirtualDisplay(req.appId)) {
        gpusApart(caps, displayId, req.previousPrimary, &media.nativeHost.virtualDisplayGpu,
                  &media.nativeHost.previousPrimaryGpu);
        if (!media.nativeHost.virtualDisplayGpu.isEmpty())
            Logger::info(
                QStringLiteral("NativeHostBackend: the virtual display is drawn by %1, "
                               "the display it took over from by %2 — apps opened "
                               "before the stream stay on the latter")
                    .arg(media.nativeHost.virtualDisplayGpu, media.nativeHost.previousPrimaryGpu));
    }
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
