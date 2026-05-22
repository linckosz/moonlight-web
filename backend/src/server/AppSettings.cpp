#include "AppSettings.h"
#include "common/Logger.h"

#include <QDir>
#include <QFile>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <dpapi.h>
#endif
#include <QJsonDocument>
#include <QJsonObject>
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
    if (it != obj.end())
        return static_cast<quint16>(it->toInt());
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

QString AppSettings::domain() const
{
    QJsonObject obj = readAll();
    return obj.value("domain").toString();
}

void AppSettings::setDomain(const QString& domain)
{
    QJsonObject obj = readAll();
    obj["domain"] = domain;
    writeAll(obj);
}

QString AppSettings::desecToken() const
{
    QJsonObject obj = readAll();
    QString raw = obj.value("desec_token").toString();

    // "auto" / empty → not encrypted, use env var
    if (raw.isEmpty() || raw == QStringLiteral("auto"))
        return raw;

    // Legacy plain-text token (no "enc:" prefix) — read as-is
    if (!raw.startsWith(QStringLiteral("enc:")))
        return raw;

    // Encrypted token: "enc:<base64_blob>"
    return decryptToken(raw.mid(4));
}

void AppSettings::setDesecToken(const QString& token)
{
    QJsonObject obj = readAll();

    // "auto" / empty → store as-is (no encryption needed)
    if (token.isEmpty() || token == QStringLiteral("auto")) {
        obj["desec_token"] = token;
    } else {
        obj["desec_token"] = QStringLiteral("enc:") + encryptToken(token);
    }
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

QString AppSettings::certPath() const
{
    QJsonObject obj = readAll();
    return obj.value("cert_path").toString();
}

void AppSettings::setCertPath(const QString& path)
{
    QJsonObject obj = readAll();
    obj["cert_path"] = path;
    writeAll(obj);
}

QString AppSettings::certExpiry() const
{
    QJsonObject obj = readAll();
    return obj.value("cert_expiry").toString();
}

void AppSettings::setCertExpiry(const QString& expiry)
{
    QJsonObject obj = readAll();
    obj["cert_expiry"] = expiry;
    writeAll(obj);
}

// Compiled-in default deSEC token.
// This is used when the user's desec_token is "auto" or empty.
// NEVER log this value.
QString AppSettings::defaultDesecToken()
{
    return qEnvironmentVariable("DESEC_TOKEN");
}

// ── Token encryption (Windows DPAPI) ───────────────────────────────────────

QString AppSettings::encryptToken(const QString& plain)
{
    if (plain.isEmpty())
        return {};

#if defined(Q_OS_WIN)
    // Convert to UTF-16 for DPAPI (DATA_BLOB uses raw bytes)
    std::wstring wide = plain.toStdWString();
    DWORD cbIn = static_cast<DWORD>(wide.size() * sizeof(wchar_t));

    DATA_BLOB inBlob;
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(wide.data()));
    inBlob.cbData = cbIn;

    DATA_BLOB outBlob;
    ZeroMemory(&outBlob, sizeof(outBlob));

    // Encrypt with DPAPI (machine-level, current user)
    if (CryptProtectData(&inBlob, L"MWServer deSEC token",
                         nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &outBlob))
    {
        QByteArray encrypted = QByteArray::fromRawData(
            reinterpret_cast<const char*>(outBlob.pbData),
            static_cast<int>(outBlob.cbData));
        LocalFree(outBlob.pbData);
        return QString::fromLatin1(encrypted.toBase64());
    }

    qWarning("[AppSettings] CryptProtectData failed — storing token as plain text");
    return plain;
#else
    // Non-Windows: store as base64 (not encrypted — DPAPI not available)
    return QString::fromLatin1(plain.toUtf8().toBase64());
#endif
}

QString AppSettings::decryptToken(const QString& encoded)
{
    if (encoded.isEmpty())
        return {};

#if defined(Q_OS_WIN)
    QByteArray encrypted = QByteArray::fromBase64(encoded.toLatin1());

    DATA_BLOB inBlob;
    inBlob.pbData = reinterpret_cast<BYTE*>(encrypted.data());
    inBlob.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB outBlob;
    ZeroMemory(&outBlob, sizeof(outBlob));

    // Decrypt
    if (CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &outBlob))
    {
        QString result = QString::fromWCharArray(
            reinterpret_cast<const wchar_t*>(outBlob.pbData),
            static_cast<int>(outBlob.cbData / sizeof(wchar_t)));
        LocalFree(outBlob.pbData);
        return result;
    }

    // Decrypt failed — might be old base64-encoded (non-encrypted) format
    qWarning("[AppSettings] CryptUnprotectData failed — trying legacy base64");
    return QString::fromUtf8(QByteArray::fromBase64(encoded.toLatin1()));
#else
    // Non-Windows: decode base64
    return QString::fromUtf8(QByteArray::fromBase64(encoded.toLatin1()));
#endif
}

