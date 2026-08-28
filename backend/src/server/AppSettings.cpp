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
#include "common/Logger.h"
#include "common/PairingCrypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>

AppSettings::AppSettings()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_FilePath = dir + "/settings.json";
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
    QFile file(m_FilePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        file.close();
    } else {
        Logger::warning("[AppSettings] Failed to write " + m_FilePath);
    }
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

// ── Statistics consent ───────────────────────────────────────────────────────
// Nothing is reported until this says yes. Recorded the way the Internet Access
// consent is — wording, timestamp, entry point — because "we asked and they
// agreed" is only worth anything if what they agreed to was written down.

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

bool AppSettings::updateRelayAllowed() const
{
    return metricsConsentDecision() == QLatin1String("granted") && updateRelayEnabled();
}

bool AppSettings::sessionMetricsAllowed() const
{
    return metricsConsentDecision() == QLatin1String("granted") && sessionMetricsEnabled();
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
    if (!obj.contains("stream_worker_enabled")) {
        obj["stream_worker_enabled"] = true;
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
    return obj.value("stream_fps").toInt(60);
}

void AppSettings::setStreamFps(int fps)
{
    QJsonObject obj = readAll();
    // Clamp to allowed range (30–240) and round to nearest valid value
    if (fps < 1) fps = 1;
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

QString AppSettings::registeredUid() const
{
    QJsonObject obj = readAll();
    return obj.value("registered_uid").toString();
}

void AppSettings::setRegisteredUid(const QString& id)
{
    QJsonObject obj = readAll();
    obj["registered_uid"] = id;
    writeAll(obj);
}

bool AppSettings::isValidFqdn(const QString& domain)
{
    static const QRegularExpression re(
        QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9.-]*\\.[a-zA-Z0-9.-]*[a-zA-Z0-9]$"));
    return re.match(domain).hasMatch();
}

QString AppSettings::domain() const
{
    // Compute the default domain from unique_id + base domain
    QString baseDomain = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    if (baseDomain.isEmpty()) baseDomain = QStringLiteral("moonlightweb.top");

    QString uid = uniqueId();
    QString computed = uid.isEmpty() ? baseDomain : (uid + QLatin1Char('.') + baseDomain);

    // If stored domain is a real FQDN different from the default → custom domain
    QJsonObject obj = readAll();
    QString stored = obj.value("domain").toString();
    if (!stored.isEmpty() && stored != QStringLiteral("MW_DOMAIN") && isValidFqdn(stored) &&
        stored != computed)
        return stored;

    // The computed {unique_id}.{MW_DOMAIN} form only exists for an instance
    // that actually registered it under the retiring DNS mechanism. A fresh
    // install has a unique_id (it seeds the deterministic UPnP fallback port)
    // but no public name: returning the subdomain here would make HttpServer
    // trust a Host header nothing ever points at, and hand entry points a URL
    // that does not resolve.
    if (registeredUid().isEmpty()) return {};

    return computed;
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

bool AppSettings::pendingRegistration() const
{
    QJsonObject obj = readAll();
    return obj.value("pending_registration").toBool(false);
}

void AppSettings::setPendingRegistration(bool pending)
{
    QJsonObject obj = readAll();
    obj["pending_registration"] = pending;
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

// ── DNS subdomain ownership token ───────────────────────────────────────────────

QString AppSettings::ownerToken() const
{
    QJsonObject obj = readAll();
    return obj.value("owner_token").toString();
}

void AppSettings::setOwnerToken(const QString& token)
{
    QJsonObject obj = readAll();
    obj["owner_token"] = token;
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
