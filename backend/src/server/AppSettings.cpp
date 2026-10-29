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

#include "AppSettings.h"
#include "common/Edition.h"
#include "common/Logger.h"
#include "common/PairingCrypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QHash>
#include <QSet>
#include <QUuid>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString g_FileOverride;
}

AppSettings::AppSettings()
{
    if (!g_FileOverride.isEmpty()) {
        // Created on first write like the default one, so its directory has
        // to exist too: QSaveFile does not make it.
        QDir().mkpath(QFileInfo(g_FileOverride).absolutePath());
        m_FilePath = g_FileOverride;
        return;
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_FilePath = dir + "/settings.json";
}

void AppSettings::setFileOverride(const QString& path)
{
    g_FileOverride = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
}

QString AppSettings::fileOverride()
{
    return g_FileOverride;
}

// ── Low-level helpers ──────────────────────────────────────────────────────────

QJsonObject AppSettings::readAll() const
{
    QFile file(m_FilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    return obj;
}

void AppSettings::writeAll(const QJsonObject& obj)
{
    // QSaveFile, not a plain truncate-and-rewrite: this file is read by other
    // processes while we run — the Windows installer reads it to decide which
    // wizard pages are already answered — and a truncating write leaves it empty
    // for as long as the write takes. A reader landing in that window sees a
    // settings file with nothing in it and concludes the settings are not there.
    // The same window is what a crash or a power cut turns into a permanent loss
    // of every setting. QSaveFile writes a sibling temporary file and renames it
    // over this one, so the path never holds anything but a complete document.
    QSaveFile file(m_FilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::warning("[AppSettings] Failed to write " + m_FilePath);
        return;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    // commit() is where the rename happens; without it the destructor cancels
    // the write and the file is silently left untouched.
    if (!file.commit())
        Logger::warning("[AppSettings] Failed to write " + m_FilePath + ": " + file.errorString());
}

// ── HTTP port ──────────────────────────────────────────────────────────────────

quint16 AppSettings::httpPort(quint16 fallback) const
{
    QJsonObject obj = readAll();
    auto it = obj.find("http_port");
    if (it != obj.end()) return static_cast<quint16>(it->toInt());
    return fallback;
}

void AppSettings::setHttpPort(quint16 port)
{
    QJsonObject obj = readAll();
    obj["http_port"] = static_cast<int>(port);
    writeAll(obj);
}

// ── HTTPS port ─────────────────────────────────────────────────────────────────

quint16 AppSettings::httpsPort(quint16 fallback) const
{
    QJsonObject obj = readAll();
    auto it = obj.find("https_port");
    if (it != obj.end()) {
        int v = it->toInt();
        if (v > 0) // 0 is invalid — use fallback
            return static_cast<quint16>(v);
    }
    return fallback;
}

void AppSettings::setHttpsPort(quint16 port)
{
    QJsonObject obj = readAll();
    obj["https_port"] = static_cast<int>(port);
    writeAll(obj);
}

// ── Video codec ────────────────────────────────────────────────────────────────

QString AppSettings::videoCodecToString(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::Auto: return "auto";
    case VideoCodec::H264: return "h264";
    case VideoCodec::HEVC: return "hevc";
    case VideoCodec::AV1: return "av1";
    }
    return "auto";
}

VideoCodec AppSettings::videoCodecFromString(const QString& str)
{
    QString lower = str.trimmed().toLower();
    if (lower == "h264") return VideoCodec::H264;
    if (lower == "hevc") return VideoCodec::HEVC;
    if (lower == "av1") return VideoCodec::AV1;
    return VideoCodec::Auto; // default / fallback for "auto" or unknown
}

VideoCodec AppSettings::videoCodec() const
{
    QJsonObject obj = readAll();
    auto it = obj.find("video_codec");
    if (it == obj.end()) return VideoCodec::Auto;
    return videoCodecFromString(it->toString());
}

void AppSettings::setVideoCodec(VideoCodec codec)
{
    QJsonObject obj = readAll();
    obj["video_codec"] = videoCodecToString(codec);
    writeAll(obj);
}

// ── Gaming mode ────────────────────────────────────────────────────────────────

bool AppSettings::gamingMode() const
{
    QJsonObject obj = readAll();
    return obj.value("gaming_mode").toBool(false);
}

void AppSettings::setGamingMode(bool enabled)
{
    QJsonObject obj = readAll();
    obj["gaming_mode"] = enabled;
    writeAll(obj);
}

// ── First-run setup ────────────────────────────────────────────────────────────

bool AppSettings::setupCompleted() const
{
    QJsonObject obj = readAll();
    return obj.value("setup_completed").toBool(false);
}

void AppSettings::setSetupCompleted(bool completed)
{
    QJsonObject obj = readAll();
    obj["setup_completed"] = completed;
    writeAll(obj);
}

// ── Audio time-stretch (WSOLA) ─────────────────────────────────────────────────

bool AppSettings::audioTimeStretch() const
{
    QJsonObject obj = readAll();
    return obj.value("audio_time_stretch").toBool(true);
}

// ── Stream worker (per-session child process) ─────────────────────────────────
// Hosts each streaming session in a `--stream-worker` subprocess, enabling two
// concurrent sessions (dual-stream seamless switching) despite the process-
// global moonlight-common-c. File-only kill switch: set false to force the
// legacy in-process single-session path.

bool AppSettings::streamWorkerEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_worker_enabled").toBool(true);
}

// ── Native host ──────────────────────────────────────────────────────────────
// Whether this machine offers itself as a host. False hides the native host
// card without touching the engine: MoonlightWeb then only shows the hosts the
// user added (Sunshine, Wolf, …).

bool AppSettings::nativeHostEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("native_host_enabled").toBool(true);
}

void AppSettings::setNativeHostEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["native_host_enabled"] = enabled;
    writeAll(obj);
}

// ── Update relay ─────────────────────────────────────────────────────────────
// Routes the update check through https://updates.{MW_DOMAIN}, which mirrors the
// GitHub release and counts installed versions in aggregate. Opt-out, file-only;
// MW_NO_TELEMETRY in the environment overrides it (UpdateChecker reads both).

bool AppSettings::updateRelayEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("update_relay_enabled").toBool(true);
}

// Aggregate session counts (resolution/fps/codec/transport/duration), never a
// host name, an account or an application. Opt-out, file-only; MW_NO_TELEMETRY
// in the environment overrides it (SessionMetrics reads both).

bool AppSettings::sessionMetricsEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("session_metrics_enabled").toBool(true);
}

// ── Statistics consent — dormant ─────────────────────────────────────────────
// Kept whole, wired to nothing. No census gates on it any more (see
// sessionMetricsAllowed below for why), so this is the machinery waiting for
// the day a field genuinely needs an answer before it may travel: the record
// that stores the wording, when it was answered and through which entry point,
// and the version rule that refuses to let an answer given to a narrower
// question cover a wider one. Recorded the way the Internet Access consent is,
// because "we asked and they agreed" is only worth anything if what they
// agreed to was written down. Do not delete it to tidy up: rebuilding it
// correctly costs more than leaving it here costs.

QJsonObject AppSettings::metricsConsent() const
{
    QJsonObject obj = readAll();
    return obj.value("metrics_consent").toObject();
}

void AppSettings::setMetricsConsent(bool granted, const QString& message, const QString& source)
{
    QJsonObject consent;
    consent["decision"] = granted ? QStringLiteral("granted") : QStringLiteral("denied");
    consent["message"] = message;
    consent["at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    consent["source"] = source;
    consent["version"] = kMetricsConsentVersion;

    QJsonObject obj = readAll();
    obj["metrics_consent"] = consent;
    writeAll(obj);
}

QString AppSettings::metricsConsentDecision() const
{
    const QJsonObject consent = metricsConsent();
    if (consent.isEmpty()) return {};
    // An answer given to an older, narrower question does not cover a wording
    // that describes more: treat it as unasked and put the question again.
    if (consent.value("version").toInt(0) < kMetricsConsentVersion) return {};
    const QString decision = consent.value("decision").toString();
    if (decision != QLatin1String("granted") && decision != QLatin1String("denied")) return {};
    return decision;
}

bool AppSettings::sessionLocationEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("session_location_enabled").toBool(false);
}

void AppSettings::setSessionLocationEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["session_location_enabled"] = enabled;
    writeAll(obj);
}

// ── What actually governs the two censuses ───────────────────────────────────
// The switch, and nothing else. Neither census carries a field that identifies
// anyone: every value is a number or a token chosen in the source, the payload
// is emitted by this process and never by a browser, no address is stored, and
// nothing is written to any viewer's device — so there is no consent to
// collect, and what is owed instead is the disclosure (shown in Settings) and
// this way of saying no. Asking for permission for data that identifies no one
// would also cost the honest answer: the question would have to be put to the
// machine's owner, which a remote viewer is not.

bool AppSettings::updateRelayAllowed() const
{
    return updateRelayEnabled();
}

bool AppSettings::sessionMetricsAllowed() const
{
    return sessionMetricsEnabled();
}

void AppSettings::setMetricsReporting(bool enabled)
{
    // One answer, both censuses. The disclosure the user reads describes them
    // together — session shape on one side, version and OS on the other — so a
    // switch that silenced only half of it would not match its own label.
    QJsonObject obj = readAll();
    obj["session_metrics_enabled"] = enabled;
    obj["update_relay_enabled"] = enabled;
    writeAll(obj);
}

void AppSettings::seedDocumentedDefaults()
{
    QJsonObject obj = readAll();
    bool changed = false;
    // Seed the keys (with defaults) so they are discoverable/editable in the
    // file. Documentation lives in the README, not in the JSON (no comment keys).
    if (!obj.contains("audio_time_stretch")) {
        obj["audio_time_stretch"] = true;
        changed = true;
    }
    if (!obj.contains("latency_flag_enabled")) {
        obj["latency_flag_enabled"] = false;
        changed = true;
    }
    // A boolean until 0.4.0, seeded false: a stored value cannot tell a choice
    // from the seed, so false moves to the new default and true to the value
    // that keeps its meaning (AppSettings.h).
    {
        const QJsonValue fidelity = obj.value("keyboard_layout_fidelity");
        if (fidelity.isBool()) {
            const QString moved =
                fidelity.toBool() ? QStringLiteral("always") : QStringLiteral("auto");
            Logger::info(QString("settings.json: keyboard_layout_fidelity %1 -> \"%2\"")
                             .arg(fidelity.toBool() ? "true" : "false", moved));
            obj["keyboard_layout_fidelity"] = moved;
            changed = true;
        } else if (!fidelity.isString()) {
            obj["keyboard_layout_fidelity"] = QStringLiteral("auto");
            changed = true;
        }
    }
    if (!obj.contains("share_guests_admin_windows")) {
        obj["share_guests_admin_windows"] = false;
        changed = true;
    }
    if (!obj.contains("share_in_booth_mode")) {
        obj["share_in_booth_mode"] = false;
        changed = true;
    }
    if (!obj.contains("stream_worker_enabled")) {
        obj["stream_worker_enabled"] = true;
        changed = true;
    }
    if (!obj.contains("native_host_enabled")) {
        obj["native_host_enabled"] = true;
        changed = true;
    }
    if (!obj.contains("update_relay_enabled")) {
        obj["update_relay_enabled"] = true;
        changed = true;
    }
    if (!obj.contains("session_metrics_enabled")) {
        obj["session_metrics_enabled"] = true;
        changed = true;
    }
    if (!obj.contains("session_location_enabled")) {
        obj["session_location_enabled"] = false;
        changed = true;
    }
    // Written out so that the switch can be found in the file; nothing sets it.
    // No release before 0.4.1 wrote it, so a false in the file is someone's own.
    if (!obj.contains("ice_tcp_passive")) {
        obj["ice_tcp_passive"] = true;
        changed = true;
    }
    // The admin's codec of a native session (8f8d9307, never released): PyroWave
    // moved to each browser's Settings → Video codec, where it is chosen per
    // device — the one on Ethernet — rather than for every browser at once.
    if (obj.contains("native_video_codec")) {
        Logger::info(QString("settings.json: native_video_codec=%1 dropped — PyroWave is "
                             "chosen per browser now (Settings → Video codec)")
                         .arg(obj.value("native_video_codec").toString()));
        obj.remove("native_video_codec");
        changed = true;
    }
    if (changed) writeAll(obj);
}

// ── Stream bitrate ───────────────────────────────────────────────────────────────

int AppSettings::streamBitrate() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_bitrate").toInt(20000);
}

void AppSettings::setStreamBitrate(int kbps)
{
    QJsonObject obj = readAll();
    obj["stream_bitrate"] = qBound(5000, kbps, 150000);
    writeAll(obj);
}

// ── Stream height (resolution) ──────────────────────────────────────────────────

int AppSettings::streamHeight() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_height").toInt(1080);
}

void AppSettings::setStreamHeight(int height)
{
    QJsonObject obj = readAll();
    // Valid values: 720, 1080, 1440, 2160
    QList<int> valid = {720, 1080, 1440, 2160};
    if (!valid.contains(height)) height = 1080;
    obj["stream_height"] = height;
    writeAll(obj);
}

// ── Stream resolution mode ──────────────────────────────────────────────────────

// "host" was the short-lived choice of the 0.3.1 development builds; "auto"
// covers it, and anything unknown lands there too.
static const QStringList kResolutionModes = {"auto", "device", "custom", "fixed"};

QString AppSettings::streamResolution() const
{
    QJsonObject obj = readAll();
    const QString mode = obj.value("stream_resolution").toString("auto");
    return kResolutionModes.contains(mode) ? mode : QStringLiteral("auto");
}

void AppSettings::setStreamResolution(const QString& mode)
{
    QJsonObject obj = readAll();
    obj["stream_resolution"] = kResolutionModes.contains(mode) ? mode : QStringLiteral("auto");
    writeAll(obj);
}

bool AppSettings::streamBitrateAuto() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_bitrate_auto").toBool(true);
}

void AppSettings::setStreamBitrateAuto(bool automatic)
{
    QJsonObject obj = readAll();
    obj["stream_bitrate_auto"] = automatic;
    writeAll(obj);
}

int AppSettings::streamCustomWidth() const
{
    QJsonObject obj = readAll();
    return qBound(kCustomSizeMin, obj.value("stream_custom_width").toInt(1920), kCustomSizeMax);
}

int AppSettings::streamCustomHeight() const
{
    QJsonObject obj = readAll();
    return qBound(kCustomSizeMin, obj.value("stream_custom_height").toInt(1080), kCustomSizeMax);
}

void AppSettings::setStreamCustomSize(int width, int height)
{
    QJsonObject obj = readAll();
    obj["stream_custom_width"] = qBound(kCustomSizeMin, width, kCustomSizeMax);
    obj["stream_custom_height"] = qBound(kCustomSizeMin, height, kCustomSizeMax);
    writeAll(obj);
}

// ── Stream aspect ratio ────────────────────────────────────────────────────────

QString AppSettings::streamAspect() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_aspect").toString("auto");
}

void AppSettings::setStreamAspect(const QString& aspect)
{
    QJsonObject obj = readAll();
    // "auto" = the client probes the host's real format from the black bars
    // Sunshine encodes and asks again with it; the explicit ratios are manual
    // overrides, ordered by market share in the Settings dropdown.
    const QStringList valid = {"auto", "16:9", "16:10", "21:9", "4:3", "3:2", "32:9", "5:4"};
    obj["stream_aspect"] = valid.contains(aspect) ? aspect : QStringLiteral("auto");
    writeAll(obj);
}

// ── Stream frame rate ────────────────────────────────────────────────────────────

int AppSettings::streamFps() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_fps").toInt(0);
}

void AppSettings::setStreamFps(int fps)
{
    QJsonObject obj = readAll();
    // 0 is "Auto" — the client's own screen rate, resolved at launch. Any
    // other value is a rate to hold, clamped to what an encoder will take.
    if (fps < 0) fps = 0;
    if (fps > 240) fps = 240;
    obj["stream_fps"] = fps;
    writeAll(obj);
}

// ── HDR ──────────────────────────────────────────────────────────────────────────

bool AppSettings::hdrEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("hdr_enabled").toBool(false);
}

void AppSettings::setHdrEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["hdr_enabled"] = enabled;
    writeAll(obj);
}

// ── Mute host audio ────────────────────────────────────────────────────────────

bool AppSettings::muteHostAudio() const
{
    QJsonObject obj = readAll();
    return obj.value("mute_host_audio").toBool(true);
}

void AppSettings::setMuteHostAudio(bool enabled)
{
    QJsonObject obj = readAll();
    obj["mute_host_audio"] = enabled;
    writeAll(obj);
}

// ── Streaming notifications ────────────────────────────────────────────────────

bool AppSettings::streamNotifications() const
{
    QJsonObject obj = readAll();
    return obj.value("stream_notifications").toBool(true);
}

void AppSettings::setStreamNotifications(bool enabled)
{
    QJsonObject obj = readAll();
    obj["stream_notifications"] = enabled;
    writeAll(obj);
}

bool AppSettings::debugMode() const
{
    return readAll().value("debug_mode").toBool(false);
}

void AppSettings::setDebugMode(bool enabled)
{
    QJsonObject obj = readAll();
    obj["debug_mode"] = enabled;
    writeAll(obj);
}

bool AppSettings::verboseLogs() const
{
    return readAll().value("verbose_logs").toBool(false);
}

void AppSettings::setVerboseLogs(bool enabled)
{
    QJsonObject obj = readAll();
    obj["verbose_logs"] = enabled;
    writeAll(obj);
}

// ── Chroma 4:4:4 ───────────────────────────────────────────────────────────────

bool AppSettings::chroma444Enabled() const
{
    QJsonObject obj = readAll();
    return obj.value("chroma_444_enabled").toBool(false);
}

void AppSettings::setChroma444Enabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["chroma_444_enabled"] = enabled;
    writeAll(obj);
}

// ── Show performance stats ────────────────────────────────────────────────────────

bool AppSettings::showPerformanceStats() const
{
    QJsonObject obj = readAll();
    return obj.value("show_performance_stats").toBool(false);
}

void AppSettings::setShowPerformanceStats(bool enabled)
{
    QJsonObject obj = readAll();
    obj["show_performance_stats"] = enabled;
    writeAll(obj);
}

// ── Click-to-photon latency flag ─────────────────────────────────────────────

bool AppSettings::latencyFlagEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("latency_flag_enabled").toBool(false);
}

bool AppSettings::packetMarking() const
{
    QJsonObject obj = readAll();
    return obj.value("packet_marking").toBool(true);
}

void AppSettings::setPacketMarking(bool enabled)
{
    QJsonObject obj = readAll();
    obj["packet_marking"] = enabled;
    writeAll(obj);
}

bool AppSettings::iceTcpPassive() const
{
    QJsonObject obj = readAll();
    return obj.value("ice_tcp_passive").toBool(true);
}

bool AppSettings::debugIceDropUdp() const
{
    QJsonObject obj = readAll();
    return obj.value("debug_ice_drop_udp").toBool(false);
}

bool AppSettings::sharedFeedEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("shared_feed_enabled").toBool(true);
}

bool AppSettings::shareGuestsAdminWindows() const
{
    QJsonObject obj = readAll();
    // Only a real true opens them: a value someone typed as "true" (a string)
    // or 1 is not a decision to make a stranger administrator.
    return obj.value("share_guests_admin_windows").toBool(false);
}

bool AppSettings::shareInBoothMode() const
{
    // Only a real true, as above: letting guests into a booth is a decision.
    return readAll().value("share_in_booth_mode").toBool(false);
}

QString AppSettings::keyboardLayoutFidelity() const
{
    const QJsonValue value = readAll().value("keyboard_layout_fidelity");
    // Read as the startup migration writes it, for a file not yet migrated.
    if (value.isBool()) return value.toBool() ? QStringLiteral("always") : QStringLiteral("auto");
    const QString text = value.toString().trimmed().toLower();
    if (text == QLatin1String("always") || text == QLatin1String("never")) return text;
    return QStringLiteral("auto");
}

bool AppSettings::keyboardHostLayout() const
{
    // Only a real true: typing on another layout's keys is a decision.
    return readAll().value("keyboard_host_layout").toBool(false);
}

void AppSettings::setKeyboardHostLayout(bool enabled)
{
    QJsonObject obj = readAll();
    obj["keyboard_host_layout"] = enabled;
    writeAll(obj);
}

bool AppSettings::keyboardDebug() const
{
    QJsonObject obj = readAll();
    return obj.value("keyboard_debug").toBool(false);
}

QString AppSettings::nativeTuning() const
{
    QJsonObject obj = readAll();
    return obj.value("native_tuning").toString().trimmed();
}

QString AppSettings::rtpVideo() const
{
    QJsonObject obj = readAll();
    return obj.value("rtp_video").toString().trimmed();
}

// ── Router ports ─────────────────────────────────────────────────────────────
// What the allocator obtained last time. Read and rewritten as a whole object
// so a partial write can never leave the tunnel list and the media map
// disagreeing about which host address they were obtained under.

static const char kRouterPortsKey[] = "router_ports";

// ── Virtual display ───────────────────────────────────────────────────────────

static const char kVirtualDisplayKey[] = "virtual_display";

QJsonObject AppSettings::virtualDisplay() const
{
    return readAll().value(kVirtualDisplayKey).toObject();
}

void AppSettings::setVirtualDisplay(const QJsonObject& record)
{
    QJsonObject obj = readAll();
    obj[kVirtualDisplayKey] = record;
    writeAll(obj);
}

void AppSettings::clearVirtualDisplay()
{
    QJsonObject obj = readAll();
    if (!obj.contains(kVirtualDisplayKey)) return;
    obj.remove(kVirtualDisplayKey);
    writeAll(obj);
}

bool AppSettings::virtualDisplayExclusive() const
{
    QJsonObject obj = readAll();
    return obj.value("virtual_display_exclusive").toBool(false);
}

void AppSettings::setVirtualDisplayExclusive(bool enabled)
{
    QJsonObject obj = readAll();
    obj["virtual_display_exclusive"] = enabled;
    writeAll(obj);
}

// ── The native host's display cards ──────────────────────────────────────────

static const char kDisplayAppIdsKey[] = "display_app_ids";

QList<int> AppSettings::displayAppIds(const QStringList& keys, const QList<int>& firstIds)
{
    QJsonObject obj = readAll();
    QJsonObject kept = obj.value(kDisplayAppIdsKey).toObject();
    const auto valid = [](int id) { return id >= 1 && id < kDisplayAppIdEnd; };
    QSet<int> taken;
    for (auto it = kept.constBegin(); it != kept.constEnd(); ++it)
        if (valid(it.value().toInt())) taken.insert(it.value().toInt());
    QSet<QString> here;
    for (const QString& key : keys)
        here.insert(key.toLower());

    QList<int> ids;
    ids.reserve(keys.size());
    bool changed = false;
    for (int i = 0; i < keys.size(); ++i) {
        const QString key = keys.at(i).toLower();
        int id = key.isEmpty() ? 0 : kept.value(key).toInt();
        if (key.isEmpty() || valid(id)) {
            ids.append(key.isEmpty() ? 0 : id);
            continue;
        }
        id = firstIds.value(i);
        if (!valid(id) || taken.contains(id)) {
            id = 0;
            for (int free = 1; free < kDisplayAppIdEnd && !id; ++free)
                if (!taken.contains(free)) id = free;
        }
        if (!id) {
            // A screen for every id: the lowest one of a screen not here now
            // is given up.
            QString gone;
            for (auto it = kept.constBegin(); it != kept.constEnd(); ++it) {
                const int theirs = it.value().toInt();
                if (here.contains(it.key()) || !valid(theirs) || (id && theirs > id)) continue;
                id = theirs;
                gone = it.key();
            }
            if (!id) {
                ids.append(0);
                continue;
            }
            kept.remove(gone);
        }
        kept[key] = id;
        taken.insert(id);
        ids.append(id);
        changed = true;
    }
    if (changed) {
        obj[kDisplayAppIdsKey] = kept;
        writeAll(obj);
    }
    return ids;
}

QList<quint16> AppSettings::rememberedTunnelPorts() const
{
    QList<quint16> ports;
    const QJsonArray arr = readAll().value(kRouterPortsKey).toObject().value("tunnel").toArray();
    for (const QJsonValue& v : arr) {
        const int port = v.toInt();
        if (port > 0 && port <= 65535 && !ports.contains(static_cast<quint16>(port)))
            ports.append(static_cast<quint16>(port));
    }
    return ports;
}

void AppSettings::rememberTunnelPort(quint16 port)
{
    if (port == 0) return;
    QList<quint16> ports = rememberedTunnelPorts();
    if (ports.contains(port)) return;
    ports.append(port);

    QJsonObject obj = readAll();
    QJsonObject rp = obj.value(kRouterPortsKey).toObject();
    QJsonArray arr;
    for (const quint16 p : ports)
        arr.append(static_cast<int>(p));
    rp["tunnel"] = arr;
    obj[kRouterPortsKey] = rp;
    writeAll(obj);
}

void AppSettings::forgetTunnelPort(quint16 port)
{
    QList<quint16> ports = rememberedTunnelPorts();
    if (!ports.removeAll(port)) return;

    QJsonObject obj = readAll();
    QJsonObject rp = obj.value(kRouterPortsKey).toObject();
    QJsonArray arr;
    for (const quint16 p : ports)
        arr.append(static_cast<int>(p));
    rp["tunnel"] = arr;
    obj[kRouterPortsKey] = rp;
    writeAll(obj);
}

quint16 AppSettings::rememberedMediaPort(int slot) const
{
    const QJsonObject media = readAll().value(kRouterPortsKey).toObject().value("media").toObject();
    const int port = media.value(QString::number(slot)).toInt();
    return (port > 0 && port <= 65535) ? static_cast<quint16>(port) : 0;
}

void AppSettings::rememberMediaPort(int slot, quint16 port)
{
    if (port == 0 || slot < 0) return;
    if (rememberedMediaPort(slot) == port) return;

    QJsonObject obj = readAll();
    QJsonObject rp = obj.value(kRouterPortsKey).toObject();
    QJsonObject media = rp.value("media").toObject();
    media[QString::number(slot)] = static_cast<int>(port);
    rp["media"] = media;
    obj[kRouterPortsKey] = rp;
    writeAll(obj);
}

void AppSettings::forgetMediaPort(int slot)
{
    QJsonObject obj = readAll();
    QJsonObject rp = obj.value(kRouterPortsKey).toObject();
    QJsonObject media = rp.value("media").toObject();
    if (!media.contains(QString::number(slot))) return;
    media.remove(QString::number(slot));
    rp["media"] = media;
    obj[kRouterPortsKey] = rp;
    writeAll(obj);
}

QString AppSettings::routerLanIp() const
{
    return readAll().value(kRouterPortsKey).toObject().value("lan_ip").toString();
}

void AppSettings::setRouterLanIp(const QString& ip)
{
    if (routerLanIp() == ip) return;
    QJsonObject obj = readAll();
    QJsonObject rp = obj.value(kRouterPortsKey).toObject();
    rp["lan_ip"] = ip;
    obj[kRouterPortsKey] = rp;
    writeAll(obj);
}

// ── STUN server ──────────────────────────────────────────────────────────────────

QString AppSettings::stunServer() const
{
    QJsonObject obj = readAll();
    const QString configured = obj.value("stun_server").toString();
    if (!configured.isEmpty()) return configured;

    // The introduction server answers STUN on 3478 as well, so the browser is
    // told about the same operator the consent text already names instead of
    // Google. StunClient::defaultServers() keeps the public servers behind it
    // for the backend's own detection; a browser gets one server and a direct
    // connection it can fall back to.
    QString domain = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    if (domain.isEmpty()) domain = QStringLiteral("moonlightweb.top");
    return QStringLiteral("stun:stream.") + domain + QStringLiteral(":3478");
}

void AppSettings::setStunServer(const QString& url)
{
    QJsonObject obj = readAll();
    obj["stun_server"] = url;
    writeAll(obj);
}

// ── HMAC key ────────────────────────────────────────────────────────────────────

QByteArray AppSettings::hmacKey() const
{
    QJsonObject obj = readAll();
    QString b64 = obj.value("hmac_key").toString();
    if (b64.isEmpty()) return {};
    return QByteArray::fromBase64(b64.toUtf8());
}

void AppSettings::setHmacKey(const QByteArray& key)
{
    QJsonObject obj = readAll();
    obj["hmac_key"] = QString::fromLatin1(key.toBase64());
    writeAll(obj);
}

// ── UPnP NAT traversal ────────────────────────────────────────────────────────

bool AppSettings::upnpEnabled() const
{
    QJsonObject obj = readAll();
    // Default to true — UPnP is beneficial for most users
    return obj.value("upnp_enabled").toBool(true);
}

void AppSettings::setUpnpEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["upnp_enabled"] = enabled;
    writeAll(obj);
}

// ── Internet Access ───────────────────────────────────────────────────────────

bool AppSettings::internetAccessEnabled() const
{
    // Off on a LAN-only instance whatever the file says: a settings.json
    // carried over from an install that had it on must open nothing here.
    if (mw::edition::lanOnly()) return false;
    QJsonObject obj = readAll();
    return obj.value("internet_access_enabled").toBool(false);
}

void AppSettings::setInternetAccessEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["internet_access_enabled"] = enabled;
    writeAll(obj);
}

QString AppSettings::uniqueId() const
{
    QJsonObject obj = readAll();
    return obj.value("unique_id").toString();
}

void AppSettings::setUniqueId(const QString& id)
{
    QJsonObject obj = readAll();
    obj["unique_id"] = id;
    writeAll(obj);
}

// ── Desktop portal consent ───────────────────────────────────────────────────

namespace {
QString portalTokenKey(bool virtualDisplay)
{
    return virtualDisplay ? QStringLiteral("portal_virtual_restore_token")
                          : QStringLiteral("portal_restore_token");
}
} // namespace

QString AppSettings::portalRestoreToken(bool virtualDisplay) const
{
    QJsonObject obj = readAll();
    return obj.value(portalTokenKey(virtualDisplay)).toString();
}

void AppSettings::setPortalRestoreToken(const QString& token, bool virtualDisplay)
{
    const QString key = portalTokenKey(virtualDisplay);
    QJsonObject obj = readAll();
    // Rewriting the same token would be a settings file written on every
    // session for nothing. The portal only issues a new one when it actually
    // asked, so equality here is the normal case on a machine that is working.
    if (obj.value(key).toString() == token) return;
    obj[key] = token;
    writeAll(obj);
}

// ── Display rates the capture did not hold ───────────────────────────────────

QJsonArray AppSettings::vddMatchRefused() const
{
    return readAll().value(QStringLiteral("vdd_match_refused")).toArray();
}

int AppSettings::addVddMatchRefusal(const QString& gpu, int milliHz, const QString& version,
                                    qint64 nowMs)
{
    if (milliHz <= 0) return 0;
    QJsonObject obj = readAll();
    QJsonArray kept;
    int count = 1;
    for (const QJsonValue& v : obj.value(QStringLiteral("vdd_match_refused")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QLatin1String("version")).toString() != version) continue;
        if (o.value(QLatin1String("gpu")).toString() == gpu &&
            o.value(QLatin1String("milli_hz")).toInteger() == milliHz) {
            // Refused again once its hold ran out: one more in a row, held
            // twice as long. Bounded, so the count never outgrows its doubling.
            count = qMin(16, static_cast<int>(o.value(QLatin1String("count")).toInteger(1)) + 1);
            continue;
        }
        kept.append(o);
    }
    // A handful at most: one per GPU and rate a client asked of this host.
    while (kept.size() >= 8)
        kept.removeFirst();
    kept.append(QJsonObject{{QStringLiteral("gpu"), gpu},
                            {QStringLiteral("milli_hz"), milliHz},
                            {QStringLiteral("version"), version},
                            {QStringLiteral("at"), nowMs},
                            {QStringLiteral("count"), count}});
    obj[QStringLiteral("vdd_match_refused")] = kept;
    writeAll(obj);
    return count;
}

void AppSettings::clearVddMatchRefusal(const QString& gpu, int milliHz)
{
    QJsonObject obj = readAll();
    const QJsonArray before = obj.value(QStringLiteral("vdd_match_refused")).toArray();
    QJsonArray kept;
    for (const QJsonValue& v : before) {
        const QJsonObject o = v.toObject();
        if (o.value(QLatin1String("gpu")).toString() == gpu &&
            o.value(QLatin1String("milli_hz")).toInteger() == milliHz)
            continue;
        kept.append(o);
    }
    if (kept.size() == before.size()) return; // nothing refused: no file written for nothing
    obj[QStringLiteral("vdd_match_refused")] = kept;
    writeAll(obj);
}

// ── Instance name ────────────────────────────────────────────────────────────

QString AppSettings::instanceName() const
{
    QJsonObject obj = readAll();
    return obj.value("instance_name").toString().trimmed();
}

void AppSettings::setInstanceName(const QString& name)
{
    // Collapsed and clipped rather than rejected. What arrives here is a person
    // typing a label for their own machine, and a name that is merely too long
    // or padded is not a mistake worth refusing — it is one worth tidying. Line
    // breaks and control characters go because this is drawn in a header.
    QString clean;
    clean.reserve(name.size());
    for (const QChar c : name) {
        if (c.isSpace())
            clean.append(QLatin1Char(' '));
        else if (c.category() != QChar::Other_Control)
            clean.append(c);
    }
    clean = clean.simplified().left(kInstanceNameMaxLength).trimmed();

    QJsonObject obj = readAll();
    // Empty is a value, not an absence: it is how the owner says "go back to
    // whatever this PC is called", and the key stays so the file shows the
    // setting exists.
    obj["instance_name"] = clean;
    writeAll(obj);
}

QString AppSettings::machineName()
{
#ifdef Q_OS_WIN
    // GetComputerNameW rather than QHostInfo: on Windows the NetBIOS name is
    // what the machine is called in every other place its owner sees it, and it
    // is not always what DNS answers with.
    wchar_t buf[256];
    DWORD sz = static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t));
    if (GetComputerNameW(buf, &sz)) return QString::fromWCharArray(buf, static_cast<int>(sz));
    return qEnvironmentVariable("COMPUTERNAME", QStringLiteral("PC"));
#else
    const QString name = QHostInfo::localHostName();
    return name.isEmpty() ? QStringLiteral("PC") : name;
#endif
}

QString AppSettings::displayName() const
{
    const QString chosen = instanceName();
    return chosen.isEmpty() ? machineName() : chosen;
}

bool AppSettings::isValidFqdn(const QString& domain)
{
    static const QRegularExpression re(
        QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9.-]*\\.[a-zA-Z0-9.-]*[a-zA-Z0-9]$"));
    return re.match(domain).hasMatch();
}

QString AppSettings::domain() const
{
    // The name a pre-0.3 install would have registered under the retired DNS
    // mechanism. Computed only to be excluded below — nothing points at it.
    QString baseDomain = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    if (baseDomain.isEmpty()) baseDomain = QStringLiteral("moonlightweb.top");

    const QString uid = uniqueId();
    const QString retired = uid.isEmpty() ? baseDomain : (uid + QLatin1Char('.') + baseDomain);

    // A valid FQDN in settings.json that is NOT that retired name belongs to
    // the user: they own the zone and the certificate, and we serve it as-is.
    QJsonObject obj = readAll();
    QString stored = obj.value("domain").toString();
    if (!stored.isEmpty() && stored != QStringLiteral("MW_DOMAIN") && isValidFqdn(stored) &&
        stored != retired)
        return stored;

    // Otherwise this host has no public name. Returning the retired sub-domain
    // would make HttpServer trust a Host header nothing points at, and hand
    // entry points a URL that does not resolve.
    return {};
}

void AppSettings::setDomain(const QString& domain)
{
    QJsonObject obj = readAll();
    obj["domain"] = domain;
    writeAll(obj);
}

QString AppSettings::publicIp() const
{
    QJsonObject obj = readAll();
    return obj.value("public_ip").toString();
}

void AppSettings::setPublicIp(const QString& ip)
{
    QJsonObject obj = readAll();
    obj["public_ip"] = ip;
    writeAll(obj);
}

bool AppSettings::autoIpDetection() const
{
    QJsonObject obj = readAll();
    return obj.value("auto_ip_detection").toBool(true);
}

void AppSettings::setAutoIpDetection(bool enabled)
{
    QJsonObject obj = readAll();
    obj["auto_ip_detection"] = enabled;
    writeAll(obj);
}

QString AppSettings::transportMode() const
{
    QJsonObject obj = readAll();
    QString t = obj.value("transport_mode").toString();
    if (t.isEmpty()) return "auto";
    return t;
}

void AppSettings::setTransportMode(const QString& mode)
{
    QJsonObject obj = readAll();
    obj["transport_mode"] = mode;
    writeAll(obj);
}

namespace {

/// "auto", "d3d11" or "d3d12", lower-cased; empty for anything else.
QString videoPipelineName(const QString& value)
{
    // Windows' chains and Linux's (plan Phase 13); each OS ignores the
    // other's, which is also what happens to a settings file carried across.
    const QString v = value.trimmed().toLower();
    return v == QLatin1String("auto") || v == QLatin1String("d3d11") ||
                   v == QLatin1String("d3d12") || v == QLatin1String("vaapi") ||
                   v == QLatin1String("vulkan")
               ? v
               : QString();
}

} // namespace

QString AppSettings::nativeVideoPipeline() const
{
    const QString v = videoPipelineName(readAll().value("native_video_pipeline").toString());
    return v.isEmpty() ? QStringLiteral("auto") : v;
}

bool AppSettings::setNativeVideoPipeline(const QString& pipeline)
{
    const QString v = videoPipelineName(pipeline);
    if (v.isEmpty()) return false;
    QJsonObject obj = readAll();
    obj["native_video_pipeline"] = v;
    writeAll(obj);
    return true;
}

// ── The native host's apps ─────────────────────────────────────────────────

namespace {

const QStringList& hostAppSources()
{
    static const QStringList sources{QStringLiteral("user"), QStringLiteral("steam"),
                                     QStringLiteral("playnite"), QStringLiteral("xbox")};
    return sources;
}

bool validHostAppId(const QString& id)
{
    static const QRegularExpression uuid(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return uuid.match(id).hasMatch();
}

QString newHostAppId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/// A command of 1 to kHostAppCommandMax characters on one line, trimmed into
/// @p out; false for anything else.
bool hostAppCommand(const QJsonValue& value, QString* out)
{
    const QString command = value.toString().trimmed();
    *out = command;
    return !command.isEmpty() && command.size() <= AppSettings::kHostAppCommandMax &&
           !command.contains(QLatin1Char('\n')) && !command.contains(QLatin1Char('\r')) &&
           !command.contains(QChar(0));
}

/// The name and command of one entry, trimmed, or false with @p error.
bool hostAppFields(const QJsonValue& value, QString* name, QString* command, QString* error)
{
    const QJsonObject in = value.toObject();
    *name = in.value("name").toString().trimmed();
    if (!value.isObject() || name->isEmpty() || name->size() > AppSettings::kHostAppNameMax) {
        if (error)
            *error = QStringLiteral("each app needs a name of 1 to %1 characters")
                         .arg(AppSettings::kHostAppNameMax);
        return false;
    }
    if (!hostAppCommand(in.value("command"), command)) {
        if (error)
            *error = QStringLiteral("\"%1\" needs a command of 1 to %2 characters, on one line")
                         .arg(*name)
                         .arg(AppSettings::kHostAppCommandMax);
        return false;
    }
    return true;
}

/// One stored entry, cleaned, or an empty object when it is not valid.
QJsonObject storedHostApp(const QJsonValue& value)
{
    QString name, command;
    if (!hostAppFields(value, &name, &command, nullptr)) return {};
    const QJsonObject in = value.toObject();
    const QString id = in.value("id").toString();
    const int appId = in.value("app_id").toInt();
    if (!validHostAppId(id) || appId < 1100) return {};
    QJsonObject out{{"id", id}, {"app_id", appId}, {"name", name}, {"command", command}};
    QString quit;
    if (hostAppCommand(in.value("quit_command"), &quit)) out["quit_command"] = quit;
    const QString source = in.value("source").toString();
    out["source"] = hostAppSources().contains(source) ? source : QStringLiteral("user");
    static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{16,64}$"));
    const QString image = in.value("image").toString();
    if (hash.match(image).hasMatch()) out["image"] = image;
    return out;
}

QJsonArray storedHostApps(const QJsonObject& obj)
{
    QJsonArray out;
    QSet<int> ids;
    for (const QJsonValue& v : obj.value("host_apps").toArray()) {
        const QJsonObject entry = storedHostApp(v);
        if (entry.isEmpty() || out.size() >= AppSettings::kHostAppsMax) continue;
        // Two cards under one id would open the same program: the first wins.
        if (ids.contains(entry.value("app_id").toInt())) continue;
        ids.insert(entry.value("app_id").toInt());
        out.append(entry);
    }
    return out;
}

/// The next card id to hand out: past the counter and past every entry's.
int nextHostAppId(const QJsonObject& obj, const QJsonArray& apps)
{
    int next = (std::max)(obj.value("host_apps_next_id").toInt(), AppSettings::kHostAppIdFirst);
    for (const QJsonValue& v : apps)
        next = (std::max)(next, v.toObject().value("app_id").toInt() + 1);
    return next;
}

/// A new entry for the launcher @p detected, numbered from @p obj's counter
/// (moved on), or an empty object when @p detected is not one.
QJsonObject detectedHostApp(const QJsonObject& detected, QJsonObject& obj, const QJsonArray& apps)
{
    QString name, command;
    const QString source = detected.value("source").toString();
    if (!hostAppFields(detected, &name, &command, nullptr) || source == QLatin1String("user") ||
        !hostAppSources().contains(source))
        return {};
    const int appId = nextHostAppId(obj, apps);
    obj["host_apps_next_id"] = appId + 1;
    QJsonObject entry{{"id", newHostAppId()},
                      {"app_id", appId},
                      {"name", name},
                      {"command", command},
                      {"source", source}};
    QString quit;
    if (hostAppCommand(detected.value("quit_command"), &quit)) entry["quit_command"] = quit;
    return entry;
}

bool hasSource(const QJsonArray& apps, const QString& source)
{
    for (const QJsonValue& v : apps)
        if (v.toObject().value("source").toString() == source) return true;
    return false;
}

QStringList dismissedSources(const QJsonObject& obj)
{
    QStringList out;
    for (const QJsonValue& v : obj.value("host_apps_dismissed").toArray())
        if (hostAppSources().contains(v.toString()) && !out.contains(v.toString()))
            out.append(v.toString());
    return out;
}

} // namespace

QJsonArray AppSettings::hostApps() const
{
    return storedHostApps(readAll());
}

QJsonObject AppSettings::hostApp(int appId) const
{
    if (appId <= 0) return {};
    for (const QJsonValue& v : hostApps())
        if (v.toObject().value("app_id").toInt() == appId) return v.toObject();
    return {};
}

bool AppSettings::setHostApps(const QJsonArray& apps, QString* error, QStringList* removed)
{
    if (apps.size() > kHostAppsMax) {
        if (error) *error = QStringLiteral("%1 apps at most").arg(kHostAppsMax);
        return false;
    }
    QJsonObject obj = readAll();
    const QJsonArray before = storedHostApps(obj);
    QHash<QString, QJsonObject> known;
    for (const QJsonValue& v : before)
        known.insert(v.toObject().value("id").toString(), v.toObject());

    QJsonArray checked;
    QSet<QString> kept;
    for (const QJsonValue& v : apps) {
        QString name, command;
        if (!hostAppFields(v, &name, &command, error)) return false;
        const QString id = v.toObject().value("id").toString();
        QJsonObject entry;
        if (known.contains(id) && !kept.contains(id)) {
            // The server's own fields stay as they were: only the name and
            // the command are the page's to change.
            entry = known.value(id);
        } else {
            const int appId = nextHostAppId(obj, before);
            obj["host_apps_next_id"] = appId + 1;
            entry = QJsonObject{{"id", newHostAppId()}, {"app_id", appId}, {"source", "user"}};
        }
        entry["name"] = name;
        entry["command"] = command;
        kept.insert(entry.value("id").toString());
        checked.append(entry);
    }

    QStringList dismissed = dismissedSources(obj);
    for (const QJsonValue& v : before) {
        const QJsonObject entry = v.toObject();
        const QString id = entry.value("id").toString();
        if (kept.contains(id)) continue;
        if (removed) removed->append(id);
        // A launcher's card the owner removed stays removed: no later version
        // seeds it again.
        const QString source = entry.value("source").toString();
        if (source != QLatin1String("user") && !dismissed.contains(source) &&
            !hasSource(checked, source))
            dismissed.append(source);
    }
    obj["host_apps"] = checked;
    obj["host_apps_dismissed"] = QJsonArray::fromStringList(dismissed);
    writeAll(obj);
    return true;
}

bool AppSettings::setHostAppImage(const QString& id, const QString& hash)
{
    QJsonObject obj = readAll();
    QJsonArray apps = storedHostApps(obj);
    for (int i = 0; i < apps.size(); ++i) {
        QJsonObject entry = apps.at(i).toObject();
        if (entry.value("id").toString() != id) continue;
        if (hash.isEmpty())
            entry.remove("image");
        else
            entry["image"] = hash;
        apps[i] = entry;
        obj["host_apps"] = apps;
        writeAll(obj);
        return true;
    }
    return false;
}

QStringList AppSettings::hostAppsDismissed() const
{
    return dismissedSources(readAll());
}

QJsonObject AppSettings::restoreHostApp(const QJsonObject& detected, QString* error)
{
    QJsonObject obj = readAll();
    QJsonArray apps = storedHostApps(obj);
    const QString source = detected.value("source").toString();
    if (hasSource(apps, source)) {
        if (error) *error = QStringLiteral("that app is already on the host");
        return {};
    }
    if (apps.size() >= kHostAppsMax) {
        if (error) *error = QStringLiteral("%1 apps at most").arg(kHostAppsMax);
        return {};
    }
    const QJsonObject entry = detectedHostApp(detected, obj, apps);
    if (entry.isEmpty()) {
        if (error) *error = QStringLiteral("that launcher is not one this PC has");
        return {};
    }
    apps.append(entry);
    QStringList dismissed = dismissedSources(obj);
    dismissed.removeAll(source);
    obj["host_apps"] = apps;
    obj["host_apps_dismissed"] = QJsonArray::fromStringList(dismissed);
    writeAll(obj);
    return entry;
}

bool AppSettings::seedHostApps(const QJsonArray& detected, const QString& version,
                               QJsonArray* added)
{
    QJsonObject obj = readAll();
    if (obj.value("host_apps_seeded_version").toString() == version) return false;
    QJsonArray apps = storedHostApps(obj);
    const QStringList dismissed = dismissedSources(obj);
    for (const QJsonValue& v : detected) {
        const QString source = v.toObject().value("source").toString();
        if (hasSource(apps, source) || dismissed.contains(source) || apps.size() >= kHostAppsMax)
            continue;
        const QJsonObject entry = detectedHostApp(v.toObject(), obj, apps);
        if (entry.isEmpty()) continue;
        apps.append(entry);
        if (added) added->append(entry);
    }
    obj["host_apps"] = apps;
    obj["host_apps_seeded_version"] = version;
    writeAll(obj);
    return true;
}

void AppSettings::migrateHostApps()
{
    QJsonObject obj = readAll();
    bool changed = false;
    QJsonArray apps = obj.value("host_apps").toArray();
    if (obj.contains("gamescope_apps")) {
        // 1100 + rank was the card's id in the betas: kept, so a grid cached
        // by a browser still opens the same program.
        const QJsonArray old = obj.value("gamescope_apps").toArray();
        for (int i = 0; i < old.size() && i < kHostAppsMax; ++i) {
            QString name, command;
            if (!hostAppFields(old.at(i), &name, &command, nullptr)) continue;
            apps.append(QJsonObject{{"id", newHostAppId()},
                                    {"app_id", 1100 + i},
                                    {"name", name},
                                    {"command", command},
                                    {"source", "user"}});
        }
        obj.remove("gamescope_apps");
        Logger::info(QStringLiteral("settings.json: %1 gamescope app(s) moved to host_apps")
                         .arg(old.size()));
        changed = true;
    }
    // An entry written in by hand, without the server's fields: given them.
    for (int i = 0; i < apps.size(); ++i) {
        QJsonObject entry = apps.at(i).toObject();
        if (!entry.contains("name")) continue;
        bool fixed = false;
        if (!validHostAppId(entry.value("id").toString())) {
            entry["id"] = newHostAppId();
            fixed = true;
        }
        if (entry.value("app_id").toInt() < 1100) {
            const int appId = nextHostAppId(obj, apps);
            obj["host_apps_next_id"] = appId + 1;
            entry["app_id"] = appId;
            fixed = true;
        }
        if (fixed) {
            apps[i] = entry;
            changed = true;
        }
    }
    if (!changed) return;
    obj["host_apps"] = apps;
    writeAll(obj);
}

QString AppSettings::hostAppImagesDir() const
{
    return QFileInfo(m_FilePath).absolutePath() + QStringLiteral("/host-app-images");
}

// ── Video enhancement ───────────────────────────────────────────────────────

QString AppSettings::videoEnhancement() const
{
    QJsonObject obj = readAll();
    // Default ON when unset (fresh install): enhancement enabled with "auto" algo.
    // Only an explicit "off" disables it.
    return obj.value("video_enhancement").toString() == "off" ? "off" : "on";
}

void AppSettings::setVideoEnhancement(const QString& value)
{
    QJsonObject obj = readAll();
    obj["video_enhancement"] = (value == "on") ? "on" : "off";
    writeAll(obj);
}

QString AppSettings::videoEnhancementAlgo() const
{
    QJsonObject obj = readAll();
    QString a = obj.value("video_enhancement_algo").toString();
    return (a == "sgsr" || a == "fsr1" || a == "force2d") ? a : "auto";
}

void AppSettings::setVideoEnhancementAlgo(const QString& algo)
{
    QJsonObject obj = readAll();
    obj["video_enhancement_algo"] =
        (algo == "sgsr" || algo == "fsr1" || algo == "force2d") ? algo : "auto";
    writeAll(obj);
}

QString AppSettings::certPem() const
{
    QJsonObject obj = readAll();
    return obj.value("cert_pem").toString(QStringLiteral("MW_CERT_PEM"));
}

void AppSettings::setCertPem(const QString& value)
{
    QJsonObject obj = readAll();
    obj["cert_pem"] = value;
    writeAll(obj);
}

QString AppSettings::certKey() const
{
    QJsonObject obj = readAll();
    return obj.value("cert_key").toString(QStringLiteral("MW_CERT_KEY"));
}

void AppSettings::setCertKey(const QString& value)
{
    QJsonObject obj = readAll();
    obj["cert_key"] = value;
    writeAll(obj);
}

// ── Certificate Authentication ────────────────────────────────────────────────

QString AppSettings::certificateToken() const
{
    QJsonObject obj = readAll();
    return obj.value("certificate_token").toString();
}

void AppSettings::setCertificateToken(const QString& token)
{
    QJsonObject obj = readAll();
    obj["certificate_token"] = token;
    writeAll(obj);
}

bool AppSettings::certAuthEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("cert_auth_enabled").toBool(false);
}

// ── Remote admin password ─────────────────────────────────────────────────────

QString AppSettings::adminPasswordDigest() const
{
    QJsonObject obj = readAll();
    return obj.value("admin_password").toString();
}

void AppSettings::setAdminPasswordDigest(const QString& digest)
{
    QJsonObject obj = readAll();
    if (digest.isEmpty())
        obj.remove("admin_password");
    else
        obj["admin_password"] = digest;
    writeAll(obj);
}

bool AppSettings::remoteAdminEnabled() const
{
    QJsonObject obj = readAll();
    return obj.value("remote_admin_enabled").toBool(true);
}

void AppSettings::setRemoteAdminEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["remote_admin_enabled"] = enabled;
    writeAll(obj);
}

bool AppSettings::remoteAdminInternet() const
{
    QJsonObject obj = readAll();
    return obj.value("remote_admin_internet").toBool(false);
}

void AppSettings::setRemoteAdminInternet(bool enabled)
{
    QJsonObject obj = readAll();
    obj["remote_admin_internet"] = enabled;
    writeAll(obj);
}

void AppSettings::setCertAuthEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["cert_auth_enabled"] = enabled;
    writeAll(obj);
}

// ── Rendezvous identity ───────────────────────────────────────────────────────

QString AppSettings::rendezvousId() const
{
    QJsonObject obj = readAll();
    return obj.value("rendezvous_id").toString();
}

void AppSettings::setRendezvousId(const QString& id)
{
    QJsonObject obj = readAll();
    obj["rendezvous_id"] = id;
    // The claim belongs to the id it was granted for. Writing a new id while
    // leaving the old flag set would make the instance advertise an address the
    // server has never heard of, and it would look reachable while being
    // unreachable — the one failure mode with no visible symptom.
    obj["rendezvous_claimed"] = false;
    writeAll(obj);
}

QString AppSettings::rendezvousToken() const
{
    QJsonObject obj = readAll();
    return obj.value("rendezvous_token").toString();
}

void AppSettings::setRendezvousToken(const QString& token)
{
    QJsonObject obj = readAll();
    obj["rendezvous_token"] = token;
    writeAll(obj);
}

bool AppSettings::rendezvousClaimed() const
{
    QJsonObject obj = readAll();
    return obj.value("rendezvous_claimed").toBool(false);
}

void AppSettings::setRendezvousClaimed(bool claimed)
{
    QJsonObject obj = readAll();
    obj["rendezvous_claimed"] = claimed;
    writeAll(obj);
}

// ── Internet Access consent (legal traceability) ──────────────────────────────

QJsonObject AppSettings::internetConsent() const
{
    QJsonObject obj = readAll();
    return obj.value("internet_consent").toObject();
}

void AppSettings::setInternetConsent(const QString& message, const QString& source,
                                     const QString& mechanism)
{
    QJsonObject consent;
    consent["message"] = message;
    consent["at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    consent["source"] = source;
    // A consent record only covers the mechanism it was worded for. Records
    // written before this field exists (no "version") were obtained for the
    // original DNS mechanism and are treated as version 1.
    consent["version"] = 2;
    consent["mechanism"] = mechanism;

    QJsonObject obj = readAll();
    obj["internet_consent"] = consent;
    writeAll(obj);
}

int AppSettings::internetConsentVersion() const
{
    const QJsonObject consent = internetConsent();
    if (consent.isEmpty()) return 0;
    return consent.value("version").toInt(1);
}

// ── Host key (host-machine recognition over the public domain) ────────────────

static QString generateLocalKey()
{
    QByteArray raw(32, '\0');
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString AppSettings::localKey()
{
    QJsonObject obj = readAll();
    QString key = obj.value("local_key").toString();
    if (!key.isEmpty()) return key;

    key = generateLocalKey();
    obj["local_key"] = key;
    writeAll(obj);
    return key;
}

QString AppSettings::rotateLocalKey()
{
    QJsonObject obj = readAll();
    const QString key = generateLocalKey();
    obj["local_key"] = key;
    writeAll(obj);
    return key;
}

// ── MW-BIND-v1 host identity key ────────────────────────────────────────────────

QByteArray AppSettings::hostSigningKeyPem()
{
    QJsonObject obj = readAll();
    const QString stored = obj.value("host_signing_key").toString();
    if (!stored.isEmpty()) {
        const QByteArray pem = QByteArray::fromBase64(stored.toUtf8());
        // A key we can no longer parse is worse than no key: every browser would
        // fail to verify a host that looks live. Regenerate and let the pairings
        // rebuild rather than serving signatures nobody can check.
        if (!PairingCrypto::publicKeySpkiFromPrivatePem(pem).isEmpty()) return pem;
        qWarning() << "[AppSettings] Stored host signing key is unusable — regenerating."
                   << "Every paired browser will have to pair again.";
    }

    const QByteArray pem = PairingCrypto::generatePrivateKeyPem();
    if (pem.isEmpty()) {
        qCritical() << "[AppSettings] Failed to generate the host signing key";
        return {};
    }

    obj["host_signing_key"] = QString::fromLatin1(pem.toBase64());
    writeAll(obj);
    return pem;
}

QByteArray AppSettings::hostSigningPublicKey()
{
    return PairingCrypto::publicKeySpkiFromPrivatePem(hostSigningKeyPem());
}
