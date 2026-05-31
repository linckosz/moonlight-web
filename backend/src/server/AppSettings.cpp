#include "AppSettings.h"
#include "common/Logger.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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
    if (it != obj.end())
        return static_cast<quint16>(it->toInt());
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
    case VideoCodec::AV1:  return "av1";
    }
    return "auto";
}

VideoCodec AppSettings::videoCodecFromString(const QString& str)
{
    QString lower = str.trimmed().toLower();
    if (lower == "h264") return VideoCodec::H264;
    if (lower == "hevc") return VideoCodec::HEVC;
    if (lower == "av1")  return VideoCodec::AV1;
    return VideoCodec::Auto;  // default / fallback for "auto" or unknown
}

VideoCodec AppSettings::videoCodec() const
{
    QJsonObject obj = readAll();
    auto it = obj.find("video_codec");
    if (it == obj.end())
        return VideoCodec::Auto;
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
    // Valid values: 720, 1080, 1440, 2160, or 0 (Native Host)
    QList<int> valid = {0, 720, 1080, 1440, 2160};
    if (!valid.contains(height))
        height = 1080;
    obj["stream_height"] = height;
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
    if (fps < 30) fps = 30;
    if (fps > 240) fps = 240;
    obj["stream_fps"] = fps;
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
    return obj.value("stun_server").toString(
        QStringLiteral("stun:stun.l.google.com:19302"));
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
    if (b64.isEmpty())
        return {};
    return QByteArray::fromBase64(b64.toUtf8());
}

void AppSettings::setHmacKey(const QByteArray& key)
{
    QJsonObject obj = readAll();
    obj["hmac_key"] = QString::fromLatin1(key.toBase64());
    writeAll(obj);
}

// ── Transport preference ───────────────────────────────────────────────────────

QString AppSettings::transport() const
{
    QJsonObject obj = readAll();
    QString t = obj.value("transport").toString();
    if (t.isEmpty())
        return "webrtc";  // default
    return t;
}

void AppSettings::setTransport(const QString& transport)
{
    QJsonObject obj = readAll();
    obj["transport"] = transport;
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

bool AppSettings::isValidFqdn(const QString& domain)
{
    static const QRegularExpression re(QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9.-]*\\.[a-zA-Z0-9.-]*[a-zA-Z0-9]$"));
    return re.match(domain).hasMatch();
}

QString AppSettings::domain() const
{
    // Compute the default domain from unique_id + base domain
    QString baseDomain = QString::fromUtf8(qgetenv("MW_DOMAIN"));
    if (baseDomain.isEmpty())
        baseDomain = QStringLiteral("moonlightweb.top");

    QString uid = uniqueId();
    QString computed = uid.isEmpty() ? baseDomain : (uid + QLatin1Char('.') + baseDomain);

    // If stored domain is a real FQDN different from the default → custom domain
    QJsonObject obj = readAll();
    QString stored = obj.value("domain").toString();
    if (!stored.isEmpty() && stored != QStringLiteral("MW_DOMAIN")
        && isValidFqdn(stored) && stored != computed)
        return stored;

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
    if (t.isEmpty())
        return "auto";
    return t;
}

void AppSettings::setTransportMode(const QString& mode)
{
    QJsonObject obj = readAll();
    obj["transport_mode"] = mode;
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

void AppSettings::setCertAuthEnabled(bool enabled)
{
    QJsonObject obj = readAll();
    obj["cert_auth_enabled"] = enabled;
    writeAll(obj);
}

