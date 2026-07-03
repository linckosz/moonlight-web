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

#include <QSslConfiguration>
#include <QSslKey>
#include <QString>

/// Owns everything TLS-certificate related, extracted out of HttpServer so the
/// socket server no longer mixes network I/O with certificate discovery,
/// loading, ACME renewal and self-signed generation.
///
/// It produces two QSslConfigurations:
///   - public: the PositiveSSL/Let's Encrypt cert served to public-domain clients
///   - local:  a self-signed cert with LAN SANs served to localhost / LAN IPs (SNI)
///
/// HttpServer keeps ownership of the live listener and pushes publicConfig()
/// onto it after a reload (that wiring stays in HttpServer::applyPublicSslConfig).
class CertManager
{
public:
    // ── Inputs (from settings, set before loadCert()/reloadTls()) ─────────────
    void setDomain(const QString& domain) { m_Domain = domain; }
    QString domain() const { return m_Domain; }
    void setCertPem(const QString& value) { m_CertPem = value; }
    void setCertKey(const QString& value) { m_CertKey = value; }

    // ── Configurations produced ───────────────────────────────────────────────
    QSslConfiguration publicConfig() const { return m_SslConfig; }
    QSslConfiguration localConfig() const { return m_LocalSslConfig; }
    void setPublicConfig(const QSslConfiguration& cfg) { m_SslConfig = cfg; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    /// Discover/load the public cert (cert_pem/key sources, then scan by domain,
    /// then self-signed fallback). Populates publicConfig(). Returns true when a
    /// usable HTTPS config is available.
    bool loadCert();

    /// Regenerate the local self-signed cert (localhost + current LAN IP SANs)
    /// and load it into localConfig(). Called after loadCert() for SNI.
    void ensureLocalSslConfig();

    /// Reload the public cert from its sources/dir (hot reload after ACME
    /// issuance). Populates publicConfig(). Returns true on success. The caller
    /// is responsible for pushing the new config onto the live listener.
    bool reloadTls();

    /// Generate a fresh self-signed cert in AppData/cert/ and load it as the
    /// public config. Returns true on success.
    bool generateSelfSignedCert();

private:
    QString findCertDir();
    bool loadCertFiles(const QString& certDir);
    bool loadCertFilesExplicit(const QString& certFilePath);
    QString findCertByDomain(const QString& domain);
    QString extractCertCN(const QString& pemPath);
    QString scanKeyInDir(const QString& dir) const;
    QSslKey loadKeyFromEnv() const;
    QString scanCertInDir(const QString& dir, const QString& domain = QString()) const;
    bool renewWithLego();

    static QByteArray resolvePemValue(const QString& value);

    /// Public config (PositiveSSL/LE cert for public-domain clients).
    QSslConfiguration m_SslConfig;
    /// Self-signed config with LAN SANs (localhost / LAN IP clients, via SNI).
    QSslConfiguration m_LocalSslConfig;

    QString m_CertPem; ///< env var name or file path
    QString m_CertKey; ///< env var name or file path
    QString m_Domain;  ///< expected CN for cert matching
};
