#pragma once

#include <QString>
#include <QJsonObject>

#include "../streaming/StreamConfig.h"

// Persistent settings stored in QStandardPaths::AppDataLocation/settings.json.
//
// This class provides a type-safe interface over the JSON settings file
// and handles read/write synchronisation (single-threaded, synchronous I/O).
class AppSettings
{
public:
    explicit AppSettings();

    // ── HTTPS port ──────────────────────────────────────────────────────────

    quint16 httpsPort(quint16 fallback = 443) const;
    void setHttpsPort(quint16 port);

    // ── Video codec preference ──────────────────────────────────────────────
    //
    // Values stored in JSON: "auto", "h264", "hevc", "av1"
    // Default: VideoCodec::Auto

    VideoCodec videoCodec() const;
    void setVideoCodec(VideoCodec codec);
    static QString videoCodecToString(VideoCodec codec);
    static VideoCodec videoCodecFromString(const QString& str);

    // ── DuckDNS ─────────────────────────────────────────────────────────────

    QString ddnsToken() const;
    void setDdnsToken(const QString& token);

    /// Consentement RGPD pour la creation du sous-domaine DuckDNS.
    bool ddnsConsentGranted() const;
    void setDdnsConsentGranted(bool granted);

    // ── Low-level access (for other one-off settings) ───────────────────────

    /// Read the entire settings JSON object.
    QJsonObject readAll() const;

    /// Write back a full JSON object.
    void writeAll(const QJsonObject& obj);

    /// Full path to settings.json.
    QString filePath() const { return m_FilePath; }

private:
    QString m_FilePath;
};
