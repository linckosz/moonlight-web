#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QXmlStreamReader>

#include "NvAddress.h"
#include "NvApp.h"

struct NvDisplayMode {
    int width = 0;
    int height = 0;
    int refreshRate = 0;

    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width && height == other.height
            && refreshRate == other.refreshRate;
    }
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    // Poll deadline for serverinfo. Generous on purpose: aborting a slow-but-
    // alive Sunshine mid-response wedges its single-threaded HTTP server and
    // makes the host appear offline to co-located native clients.
    static constexpr int FAST_FAIL_TIMEOUT_MS = 5000;
    static constexpr int REQUEST_TIMEOUT_MS   = 5000;

    explicit NvHTTP(QNetworkAccessManager* nam, QObject* parent = nullptr);

    // Async server info fetch — caller owns the returned QNetworkReply
    QNetworkReply* getServerInfoAsync(const NvAddress& address, const QString& uniqueId);

    // HTTPS variant for paired hosts (port 47989) — returns real PairStatus
    QNetworkReply* getServerInfoAsyncHttps(const NvAddress& address, const QString& uniqueId,
                                            const QByteArray& clientCertPem,
                                            const QByteArray& clientKeyPem);

    // App list (HTTPS, requires client cert, async)
    QNetworkReply* getAppListAsync(const NvAddress& address, quint16 httpsPort,
                                   const QByteArray& clientCertPem,
                                   const QByteArray& clientKeyPem);

    // Launch / quit app (HTTPS, requires client cert, async)
    QNetworkReply* launchAppAsync(const NvAddress& address, quint16 httpsPort,
                                   int appId, const QString& uniqueId,
                                   const QByteArray& rikey, int rikeyid,
                                   int width, int height, int fps, int bitrate,
                                   const QByteArray& clientCertPem,
                                   const QByteArray& clientKeyPem,
                                   int hdrMode = 0);

    // Resume an existing session for this uniqueId (reconnect to our own
    // orphaned session without relaunching the app). Keyed by uniqueId so it
    // never touches another client's session.
    QNetworkReply* resumeAppAsync(const NvAddress& address, quint16 httpsPort,
                                   const QString& uniqueId,
                                   const QByteArray& rikey, int rikeyid,
                                   const QByteArray& clientCertPem,
                                   const QByteArray& clientKeyPem);

    // uniqueId empty → falls back to the shared Moonlight unique ID.
    QNetworkReply* quitAppAsync(const NvAddress& address, quint16 httpsPort,
                                 const QByteArray& clientCertPem,
                                 const QByteArray& clientKeyPem,
                                 const QString& uniqueId = QString());

    // Static XML helpers
    static void verifyResponseStatus(const QString& xml);
    static QString getXmlString(const QString& xml, const QString& tagName);
    static QByteArray getXmlStringFromHex(const QString& xml, const QString& tagName);
    static QVector<NvDisplayMode> getDisplayModeList(const QString& serverInfo);
    static int getCurrentGame(const QString& serverInfo);
    static QVector<NvApp> parseAppList(const QString& xml);
    static QVector<int> parseQuad(const QString& quad);
    static QString parseSessionUrl(const QString& launchXml);

private:
    QUrl buildUrl(const NvAddress& address, const QString& command,
                  const QString& uniqueId, const QString& arguments = QString()) const;

    QNetworkAccessManager* m_Nam;
};
