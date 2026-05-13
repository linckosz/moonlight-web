#include "AppSettings.h"
#include "common/Logger.h"

#include <QDir>
#include <QFile>
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
    return obj.value("gaming_mode", true).toBool();
}

void AppSettings::setGamingMode(bool enabled)
{
    QJsonObject obj = readAll();
    obj["gaming_mode"] = enabled;
    writeAll(obj);
}

// ── DuckDNS ────────────────────────────────────────────────────────────────────

QString AppSettings::ddnsToken() const
{
    QJsonObject obj = readAll();
    return obj.value("ddns_token").toString();
}

void AppSettings::setDdnsToken(const QString& token)
{
    QJsonObject obj = readAll();
    obj["ddns_token"] = token;
    writeAll(obj);
}

bool AppSettings::ddnsConsentGranted() const
{
    QJsonObject obj = readAll();
    return obj.value("ddns_consent_granted").toBool();
}

void AppSettings::setDdnsConsentGranted(bool granted)
{
    QJsonObject obj = readAll();
    obj["ddns_consent_granted"] = granted;
    obj["ddns_consent_version"] = 1;
    writeAll(obj);
}
