#include "DdnsClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int IP_CHECK_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes
static const char* DUCK_DNS_URL =
    "https://www.duckdns.org/update";
static const char* IP_DISCOVERY_URL =
    "https://api.ipify.org";
static const int LEGO_TIMEOUT_MS = 120000;   // 2 min for initial run
static const int MAX_LEGO_RETRIES = 2;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DdnsClient::DdnsClient(CertReloadCallback onCertReload, QObject* parent)
    : QObject(parent)
    , m_Net(new QNetworkAccessManager(this))
    , m_IpCheckTimer(new QTimer(this))
    , m_OnCertReload(std::move(onCertReload))
{
    connect(m_IpCheckTimer, &QTimer::timeout, this, &DdnsClient::checkIp);
}

DdnsClient::~DdnsClient() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DdnsClient::start()
{
    QJsonObject settings = loadSettings();

    // ---- GDPR consent check ----
    if (settings.contains("ddns_consent_granted")) {
        m_ConsentAsked = true;
        m_ConsentGiven = settings["ddns_consent_granted"].toBool();
    }

    if (!m_ConsentAsked) {
        // Dev bypass: env var MOONLIGHT_DDNS_CONSENT=1 or .moonlight-web-dev file
        // at project root silently grants consent (no installer, no UI popup).
        if (qEnvironmentVariableIntValue("MOONLIGHT_DDNS_CONSENT") == 1) {
            qInfo() << "[DdnsClient] MOONLIGHT_DDNS_CONSENT=1 — consent auto-granted (env)";
            setConsent(true);
            return;
        }
        QString devFlag = QCoreApplication::applicationDirPath() + "/../../.moonlight-web-dev";
        if (QFile::exists(devFlag)) {
            qInfo() << "[DdnsClient] .moonlight-web-dev found — consent auto-granted (file)";
            setConsent(true);
            return;
        }
        // No bypass — ask for consent via UI
        emit consentRequired();
        return;
    }

    if (!m_ConsentGiven) {
        // User declined — silently do nothing
        return;
    }

    // ---- Load or generate DuckDNS config ----
    if (settings.contains("ddns_subdomain")) {
        m_Subdomain = settings["ddns_subdomain"].toString();
    } else {
        // First time — generate and persist a stable subdomain
        m_Subdomain = generateSubdomain();
        settings["ddns_subdomain"] = m_Subdomain;
        saveSettings(settings);
    }

    m_Token = settings["ddns_token"].toString();

    // Run the initial IP discovery + registration
    checkIp();

    // Start periodic IP check
    m_IpCheckTimer->start(IP_CHECK_INTERVAL_MS);
}

void DdnsClient::setConsent(bool granted)
{
    if (m_ConsentAsked)
        return;  // already decided

    m_ConsentAsked = true;
    m_ConsentGiven = granted;

    QJsonObject settings = loadSettings();
    settings["ddns_consent_granted"] = granted;
    settings["ddns_consent_version"] = 1;
    saveSettings(settings);

    if (granted) {
        // Re-enter start() to continue the workflow
        start();
    }
}

void DdnsClient::configure(const QString& token)
{
    m_Token = token;
    m_Active = false;  // force re-registration
    m_CurrentIp.clear();

    // Save token immediately
    QJsonObject settings = loadSettings();
    settings["ddns_token"] = token;
    if (!settings.contains("ddns_subdomain")) {
        settings["ddns_subdomain"] = generateSubdomain();
    }
    saveSettings(settings);

    // Reload subdomain from settings
    m_Subdomain = settings["ddns_subdomain"].toString();

    qInfo() << "[DdnsClient] Configured with token, subdomain:" << m_Subdomain;

    // Run IP discovery + DuckDNS registration immediately
    checkIp();

    // Ensure periodic check is running
    if (!m_IpCheckTimer->isActive()) {
        m_IpCheckTimer->start(IP_CHECK_INTERVAL_MS);
    }
}

// ---------------------------------------------------------------------------
// IP check & DuckDNS update
// ---------------------------------------------------------------------------

void DdnsClient::checkIp()
{
    QString ip = discoverPublicIp();
    if (ip.isEmpty()) {
        // Will retry on next timer tick
        return;
    }

    if (ip == m_CurrentIp && m_Active) {
        // IP unchanged and already registered — nothing to do
        return;
    }

    // IP changed (or first discovery)
    m_CurrentIp = ip;
    emit ipChanged(ip);

    qInfo() << "[DdnsClient] Public IP:" << ip;

    if (m_Token.isEmpty()) {
        qWarning() << "[DdnsClient] No DuckDNS token configured — skipping update";
        emit errorOccurred("DuckDNS token not configured");
        return;
    }

    if (!updateDuckDns(ip)) {
        qWarning() << "[DdnsClient] DuckDNS update failed for IP:" << ip;
        return;
    }

    qInfo() << "[DdnsClient] DuckDNS updated:" << m_Subdomain << "->" << ip;

    if (!m_Active) {
        // First successful registration — trigger Let's Encrypt
        m_Active = true;

        // Save the current IP so we don't re-register
        QJsonObject settings = loadSettings();
        settings["ddns_token"] = m_Token;
        settings["ddns_subdomain"] = m_Subdomain;
        settings["ddns_last_ip"] = m_CurrentIp;
        saveSettings(settings);

        emit registered(m_Subdomain, ip);

        if (m_OnCertReload) {
            if (runLego()) {
                emit certObtained();
            }
        }
    } else {
        // IP changed — save new IP
        QJsonObject settings = loadSettings();
        settings["ddns_last_ip"] = m_CurrentIp;
        saveSettings(settings);
    }
}

// ---------------------------------------------------------------------------
// Public IP discovery
// ---------------------------------------------------------------------------

QString DdnsClient::discoverPublicIp()
{
    QNetworkRequest req;
    req.setUrl(QUrl(QString::fromLatin1(IP_DISCOVERY_URL)));
    req.setTransferTimeout(10000);  // 10s timeout

    QNetworkReply* reply = m_Net->get(req);

    // Synchronous wait — this runs on startup / timer, not on the main request thread.
    // The timer fires in the Qt event loop, so we need to use a local event loop
    // to wait. Alternatively, we can use the async finished signal, but that
    // complicates the flow. Since this runs infrequently (every 5 min), a short
    // blocking wait is acceptable.
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);  // 15s safety timeout
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "[DdnsClient] IP discovery failed:" << reply->errorString();
        reply->deleteLater();
        return {};
    }

    QByteArray body = reply->readAll();
    reply->deleteLater();

    QString ip = QString::fromUtf8(body).trimmed();
    if (ip.isEmpty()) {
        qWarning() << "[DdnsClient] IP discovery returned empty response";
        return {};
    }

    // Basic validation — should be an IPv4 address
    QStringList parts = ip.split('.');
    if (parts.size() != 4) {
        qWarning() << "[DdnsClient] IP discovery returned invalid IP:" << ip;
        return {};
    }

    return ip;
}

// ---------------------------------------------------------------------------
// DuckDNS API
// ---------------------------------------------------------------------------

bool DdnsClient::updateDuckDns(const QString& ip)
{
    QUrl url(QString::fromLatin1(DUCK_DNS_URL));
    QUrlQuery query;
    query.addQueryItem("domains", m_Subdomain);
    query.addQueryItem("token", m_Token);
    query.addQueryItem("ip", ip);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setTransferTimeout(15000);

    QNetworkReply* reply = m_Net->get(req);

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "[DdnsClient] DuckDNS API error:" << reply->errorString();
        reply->deleteLater();
        return false;
    }

    QByteArray body = reply->readAll();
    reply->deleteLater();

    // DuckDNS returns "OK" on success
    bool ok = QString::fromUtf8(body).trimmed() == "OK";
    if (!ok) {
        qWarning() << "[DdnsClient] DuckDNS API returned unexpected response:" << body;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Let's Encrypt via lego (DNS-01 DuckDNS)
// ---------------------------------------------------------------------------

bool DdnsClient::runLego()
{
    // lego --dns duckdns --domains <subdomain>.duckdns.org \
    //      --email noreply@moonlight-web.local run

    QString domain = m_Subdomain + ".duckdns.org";

    // Set the DUCKDNS_TOKEN environment variable for lego
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("DUCKDNS_TOKEN", m_Token);

    QProcess lego;
    lego.setProcessEnvironment(env);
    lego.setProcessChannelMode(QProcess::MergedChannels);
    lego.setWorkingDirectory(QCoreApplication::applicationDirPath());

    QStringList args;
    args << "--dns" << "duckdns"
         << "--domains" << domain
         << "--email" << "noreply@moonlight-web.local"
         << "run";

    lego.start("lego", args);

    if (!lego.waitForStarted(10000)) {
        qWarning() << "[DdnsClient] lego not found in PATH";
        emit errorOccurred("lego not found in PATH — cannot obtain Let's Encrypt certificate");
        return false;
    }

    if (!lego.waitForFinished(LEGO_TIMEOUT_MS)) {
        lego.kill();
        lego.waitForFinished(5000);
        qWarning() << "[DdnsClient] lego timed out after" << (LEGO_TIMEOUT_MS / 1000) << "s";
        emit errorOccurred("lego timed out obtaining certificate");

        if (m_LegoRetries < MAX_LEGO_RETRIES) {
            m_LegoRetries++;
            qInfo() << "[DdnsClient] Retrying lego (attempt" << (m_LegoRetries + 1) << ")";
            return runLego();
        }
        return false;
    }

    QByteArray output = lego.readAll();

    if (lego.exitCode() != 0) {
        qWarning() << "[DdnsClient] lego failed (exit" << lego.exitCode() << "):"
                    << QString::fromUtf8(output).trimmed();
        emit errorOccurred("Let's Encrypt certificate acquisition failed");

        if (m_LegoRetries < MAX_LEGO_RETRIES) {
            m_LegoRetries++;
            qInfo() << "[DdnsClient] Retrying lego (attempt" << (m_LegoRetries + 1) << ")";
            return runLego();
        }
        return false;
    }

    m_LegoRetries = 0;
    qInfo() << "[DdnsClient] Let's Encrypt certificate obtained for" << domain;

    // Lego stores certs in ./<domain>/ by default relative to working dir.
    // The cert is at: .lego/certificates/<domain>.crt
    // We need to copy/symlink it to our cert directory.
    QString certDir = QCoreApplication::applicationDirPath() + "/cert/";
    QDir().mkpath(certDir);

    QString legoDir = QCoreApplication::applicationDirPath() + "/.lego/certificates/";
    QString certSrc = legoDir + domain + ".crt";
    QString keySrc  = legoDir + domain + ".key";

    QFile::remove(certDir + "cert.pem");
    QFile::remove(certDir + "key.pem");

    if (!QFile::copy(certSrc, certDir + "cert.pem") ||
        !QFile::copy(keySrc, certDir + "key.pem")) {
        qWarning() << "[DdnsClient] Failed to copy Let's Encrypt certificates to cert/";
        emit errorOccurred("Failed to copy Let's Encrypt certificates");
        return false;
    }

    qInfo() << "[DdnsClient] Certificates copied to" << certDir;

    // Trigger TLS reload
    if (m_OnCertReload) {
        m_OnCertReload();
    }

    return true;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

QString DdnsClient::settingsFilePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/settings.json";
}

QJsonObject DdnsClient::loadSettings()
{
    QFile file(settingsFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        // File doesn't exist yet — return empty object
        return {};
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) {
        qWarning() << "[DdnsClient] settings.json is not a JSON object — resetting";
        return {};
    }

    return doc.object();
}

bool DdnsClient::saveSettings(const QJsonObject& settings)
{
    // Merge with existing settings to preserve other keys
    QJsonObject existing = loadSettings();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        existing[it.key()] = it.value();
    }

    QFile file(settingsFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[DdnsClient] Cannot save settings:" << file.errorString();
        return false;
    }

    QJsonDocument doc(existing);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString DdnsClient::generateSubdomain() const
{
    // Generate a unique subdomain: "moonlightweb-<8-char-hex>"
    QString uuid = QUuid::createUuid().toString(QUuid::Id128);
    QString shortId = uuid.left(8).toLower();
    return "moonlightweb-" + shortId;
}
