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

#include "test_framework.h"

#include "common/LoopbackKey.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {

/// What the server sees of an upgrade: the header's value, as a socket reads it.
QByteArray headerSeenFrom(const QNetworkRequest& request, QWebSocketServer& server)
{
    QByteArray seen = "(no connection)";
    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    QObject::connect(&server, &QWebSocketServer::newConnection, &loop, [&]() {
        QWebSocket* socket = server.nextPendingConnection();
        seen = socket->request().rawHeader(mw::loopback::kHeader);
        CHECK_EQ(mw::loopback::accepts(*socket, QByteArray("right")), seen == "right");
        CHECK(!mw::loopback::accepts(*socket, QByteArray()));
        socket->close();
        socket->deleteLater();
        loop.quit();
    });
    QWebSocket client;
    client.open(request);
    loop.exec();
    QObject::disconnect(&server, &QWebSocketServer::newConnection, &loop, nullptr);
    return seen;
}

} // namespace

void run_loopback_key_tests()
{
    using namespace mw::loopback;

    SECTION("LoopbackKey — the key");
    {
        const QByteArray drawn = key();
        CHECK_EQ(drawn.size(), qsizetype(64));
        CHECK_EQ(key(), drawn);
        setKey("given");
        CHECK_EQ(key(), QByteArray("given"));
        // A worker told nothing does not end up with an empty key, or the old one.
        setKey(QByteArray());
        CHECK_EQ(key().size(), qsizetype(64));
        CHECK(key() != QByteArray("given"));
    }

    SECTION("LoopbackKey — the proxy stamps the upgrade");
    {
        const QByteArray plain = "GET /ws HTTP/1.1\r\nHost: h\r\n\r\n";
        CHECK_EQ(stampUpgrade(plain, "k"),
                 QByteArray("GET /ws HTTP/1.1\r\nHost: h\r\nX-MW-Internal-Key: k\r\n\r\n"));

        // One brought by the client goes, in any spelling.
        const QByteArray forged =
            "GET /ws HTTP/1.1\r\nx-mw-INTERNAL-key: forged\r\nHost: h\r\n\r\n";
        CHECK_EQ(stampUpgrade(forged, "k"),
                 QByteArray("GET /ws HTTP/1.1\r\nHost: h\r\nX-MW-Internal-Key: k\r\n\r\n"));

        // What followed the headers still follows them.
        CHECK(stampUpgrade(plain + "after", "k").endsWith("\r\n\r\nafter"));

        // Not a complete upgrade: untouched, so it carries no key.
        const QByteArray partial = "GET /ws HTTP/1.1\nHost: h\n\n";
        CHECK_EQ(stampUpgrade(partial, "k"), partial);
    }

    SECTION("LoopbackKey — an accepted socket reads the header");
    {
        QWebSocketServer server(QStringLiteral("test"), QWebSocketServer::NonSecureMode);
        CHECK(server.listen(QHostAddress::LocalHost, 0));
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1/").arg(server.serverPort()));

        QNetworkRequest keyed(url);
        keyed.setRawHeader(kHeader, "right");
        CHECK_EQ(headerSeenFrom(keyed, server), QByteArray("right"));

        QNetworkRequest wrong(url);
        wrong.setRawHeader(kHeader, "wrong");
        CHECK_EQ(headerSeenFrom(wrong, server), QByteArray("wrong"));

        CHECK_EQ(headerSeenFrom(QNetworkRequest(url), server), QByteArray());
    }
}
