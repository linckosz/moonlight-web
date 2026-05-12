#pragma once

#include <QObject>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <functional>

/**
 * @file DdnsClient.h
 * @brief DuckDNS dynamic DNS client for Moonlight-Web.
 *
 * Responsibilities:
 *   1. GDPR consent: ask once, store decision in settings.json.
 *   2. If consent granted, discover public IP and register with DuckDNS.
 *   3. Poll IP every 5 minutes; update DuckDNS on change.
 *   4. After successful registration, trigger Let's Encrypt cert
 *      acquisition via lego (DNS-01 challenge).
 *
 * Settings file: settings.json next to the executable.
 * DuckDNS API: https://www.duckdns.org/update?domains=...&token=...&ip=...
 */

class DdnsClient : public QObject
{
    Q_OBJECT

public:
    /// Callback when a new Let's Encrypt certificate has been obtained.
    /// The callee (HttpServer) should reload TLS config.
    using CertReloadCallback = std::function<void()>;

    explicit DdnsClient(CertReloadCallback onCertReload = nullptr,
                        QObject* parent = nullptr);
    ~DdnsClient();

    /// Start the DuckDNS workflow:
    ///   - Load settings / check consent
    ///   - If consent not yet given, emit consentRequired() — caller shows UI
    ///   - If consent granted, start IP discovery + DuckDNS registration
    ///   - If consent denied, silently do nothing
    void start();

    /// Called by the UI / frontend to submit user consent.
    /// @param granted true = user accepted, false = user declined
    void setConsent(bool granted);

    /// Configure DuckDNS token (can be called after initial setup to update the token).
    /// Saves token and re-triggers IP discovery + DuckDNS registration.
    void configure(const QString& token);

    /// Set the HTTPS port for the registered signal and log messages.
    void setHttpsPort(quint16 port) { m_HttpsPort = port; }

    /// Returns whether DuckDNS is currently active (consent granted + registered).
    bool isActive() const { return m_Active; }

    /// The DuckDNS subdomain (e.g. "moonlightweb-a1b2c3") or empty if not configured.
    QString subdomain() const { return m_Subdomain; }

    /// The DuckDNS token, or empty if not configured.
    QString token() const { return m_Token; }

signals:
    /// Emitted when GDPR consent is needed. The UI / frontend should call
    /// setConsent() in response.
    void consentRequired();

    /// Emitted when public IP changes (includes the new IP).
    void ipChanged(const QString& newIp);

    /// Emitted on error (network, API, etc.).
    void errorOccurred(const QString& message);

    /// Emitted when DuckDNS registration succeeds. Includes the HTTPS port.
    void registered(const QString& subdomain, const QString& ip, quint16 httpsPort);

    /// Emitted when a Let's Encrypt certificate has been obtained.
    void certObtained();

private slots:
    void checkIp();

private:
    QString settingsFilePath() const;
    QJsonObject loadSettings();
    bool saveSettings(const QJsonObject& settings);
    QString discoverPublicIp();
    bool updateDuckDns(const QString& ip);
    bool runLego();
    QString generateSubdomain() const;

    QNetworkAccessManager* m_Net;
    QTimer* m_IpCheckTimer;

    bool m_Active = false;
    bool m_ConsentGiven = false;
    bool m_ConsentAsked = false;
    QString m_Subdomain;
    QString m_Token;
    QString m_CurrentIp;
    quint16 m_HttpsPort = 443;
    int m_LegoRetries = 0;

    CertReloadCallback m_OnCertReload;
};
