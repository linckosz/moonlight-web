/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The hosts' network manager keeps no connection once its request ends: a
 * connection kept 120 s held the COM window of the poll's thread into the next
 * stream (D29). What is read here is the request each reply was built from.
 */
#include "test_framework.h"

#include "../src/backend/CloseWhenDoneNam.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

constexpr auto kExpiry = QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute;

// Loopback's discard port: nothing answers, nothing leaves the machine. The
// reply is aborted at once; only its request is read.
QVariant expiryOf(QNetworkAccessManager& nam, const QNetworkRequest& request)
{
    QNetworkReply* reply = nam.get(request);
    const QVariant expiry = reply->request().attribute(kExpiry);
    reply->abort();
    delete reply;
    return expiry;
}

} // namespace

void run_close_when_done_nam_tests()
{
    SECTION("CloseWhenDoneNam: a request ends its connection, unless it says otherwise");
    CloseWhenDoneNam nam;
    const QUrl url(QStringLiteral("http://127.0.0.1:9/serverinfo"));

    // A request that sets nothing: its connection goes with it.
    const QVariant unset = expiryOf(nam, QNetworkRequest(url));
    CHECK(unset.isValid());
    CHECK_EQ(unset.toInt(), 0);

    // One that sets its own expiry keeps it.
    QNetworkRequest kept(url);
    kept.setAttribute(kExpiry, 30);
    CHECK_EQ(expiryOf(nam, kept).toInt(), 30);

    // NvHTTP::closeWhenDone's own 0 stays 0.
    QNetworkRequest closing(url);
    closing.setAttribute(kExpiry, 0);
    CHECK_EQ(expiryOf(nam, closing).toInt(), 0);

    // A plain manager, for contrast: Qt's default, unset, is what kept 120 s.
    QNetworkAccessManager plain;
    CHECK(!expiryOf(plain, QNetworkRequest(url)).isValid());
}
