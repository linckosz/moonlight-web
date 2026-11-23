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

#include <QList>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QString>

/// Owns everything TLS-certificate related, extracted out of HttpServer so the
/// socket server no longer mixes network I/O with certificate discovery,
/// loading and self-signed generation.
///
/// It produces two QSslConfigurations:
///   - public: a certificate the user supplied for a domain they own
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

    /// Load the local self-signed cert (localhost + current LAN IP SANs) into
    /// localConfig(), regenerating it only when it no longer names every
    /// current address or has kLocalCertMinDaysLeft days or fewer to run.
    /// Called after loadCert() for SNI.
    void ensureLocalSslConfig();

    /// Below this many days of validity the local cert is renewed at start.
    static constexpr int kLocalCertMinDaysLeft = 30;

    /// Reload the public cert from its sources/dir (hot reload after the user
    /// replaced it). Populates publicConfig(). Returns true on success. The caller
    /// is responsible for pushing the new config onto the live listener.
    bool reloadTls();

    /// Generate a fresh self-signed cert in AppData/cert/ and load it as the
    /// public config. Returns true on success.
    bool generateSelfSignedCert();

    /// True when @p cert is valid for @p domain: the CN or any DNS subject
    /// alternative name matches, wildcards included. SANs are what browsers
    /// actually check and what a modern CA fills in — a cert whose CN is empty
    /// (increasingly common) or set to another name in the SAN list must not be
    /// rejected here. An empty @p domain matches anything.
    static bool certMatchesDomain(const QSslCertificate& cert, const QString& domain);

    /// A certificate and its key built into the binary (MW_CERT_PEM and
    /// MW_CERT_KEY at build time); empty when the build has none.
    struct BuiltInPair
    {
        QByteArray certPem;
        QByteArray keyPem;
    };
    static BuiltInPair builtInPair();

    /// The certificate and key that cert_pem (@p certPem) and cert_key
    /// (@p certKey) name. Each is an environment variable holding PEM or the
    /// path of a PEM file (a regular file of 1 MiB or less), or a file path;
    /// the build's pair (@p builtIn) stands in, as a pair, when cert_pem gives no
    /// certificate, and its key never goes with another certificate. With a certificate from
    /// a file and no key from @p certKey, the key is looked for beside it, under
    /// privkey.pem, key.pem or <the certificate's base name>.key. The
    /// certificate must cover @p domain when one is set, and the key must be
    /// the certificate's own. False, with a warning that names the sources and
    /// never their content, when anything is missing or does not match.
    /// @p described gets "cert=<source>, key=<source>".
    static bool pairFromSources(const QString& certPem, const QString& certKey,
                                const QString& domain, QList<QSslCertificate>* chain, QSslKey* key,
                                QString* described = nullptr,
                                const BuiltInPair& builtIn = builtInPair());

private:
    QString findCertDir();
    bool loadCertFiles(const QString& certDir);
    bool loadCertFilesExplicit(const QString& certFilePath);
    QString findCertByDomain(const QString& domain);
    QString extractCertCN(const QString& pemPath);
    /// Load a PEM file and report whether its first certificate covers @p domain
    /// (CN or SAN, see certMatchesDomain).
    static bool certFileMatchesDomain(const QString& pemPath, const QString& domain);
    QString scanKeyInDir(const QString& dir) const;
    /// Every *.pem under @p dir (subfolders included) that holds a private key.
    QStringList scanKeysInDir(const QString& dir) const;
    /// The first of privkey.pem, key.pem, <base name>.key beside @p certFile that
    /// is @p cert's key; empty when none is.
    static QString keyBesideCert(const QString& certFile, const QSslCertificate& cert,
                                 QSslKey* key);
    QSslKey loadKeyFromEnv() const;
    QString scanCertInDir(const QString& dir, const QString& domain = QString()) const;

    static QByteArray resolvePemValue(const QString& value, QString* filePath = nullptr);

    /// Public config (PositiveSSL/LE cert for public-domain clients).
    QSslConfiguration m_SslConfig;
    /// Self-signed config with LAN SANs (localhost / LAN IP clients, via SNI).
    QSslConfiguration m_LocalSslConfig;

    QString m_CertPem; ///< env var name or file path
    QString m_CertKey; ///< env var name or file path
    QString m_Domain;  ///< expected CN/SAN for cert matching
};
