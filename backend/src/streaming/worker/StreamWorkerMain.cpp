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

#include "StreamWorkerMain.h"

#include "../Session.h"
#include "../DataChannelRelay.h"
#include "../MediaTrackRelay.h"
#include "../StreamRelay.h"
#include "../SignalingServer.h"
#include "../MoonlightShim.h"
#include "../../backend/NvComputer.h"
#include "../../backend/NvHTTP.h"
#include "../../backend/IdentityManager.h"
#include "../../common/Types.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QDebug>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace {

/// Write one protocol event line on stdout (the only stdout writer in worker
/// mode — all logging goes to the log file / stderr).
void emitEvent(const QJsonObject& event)
{
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n";
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

/// Holds the live relay graph pointers so stdin commands and the end-of-session
/// path can tear the graph down the same way main.cpp's /quit route does.
struct WorkerState
{
    QPointer<DataChannelRelay> relay;
    QPointer<MediaTrackRelay> mediaRelay;
    QPointer<StreamRelay> streamRelay;
    bool endedEmitted = false;
    bool exiting = false;
};

WorkerState* g_State = nullptr;

/// Mirror of the /quit teardown in main.cpp: stop the shim FIRST (so moonlight
/// stops calling back into a relay about to be destroyed), then stop + delete
/// the relay. Exits the process once done (short grace for the relay thread).
void teardownAndExit(int notify /*0=none, 1=takenOver, 2=revoked*/)
{
    if (!g_State || g_State->exiting) return;
    g_State->exiting = true;

    if (g_State->relay) {
        DataChannelRelay* r = g_State->relay;
        g_State->relay = nullptr;
        if (notify == 1 && r->isConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        QObject::disconnect(r, &DataChannelRelay::sessionEnded, nullptr, nullptr);
        if (r->moonlightShim()) r->moonlightShim()->stopConnection();
        r->stop();
        r->deleteLater();
    }
    if (g_State->mediaRelay) {
        MediaTrackRelay* r = g_State->mediaRelay;
        g_State->mediaRelay = nullptr;
        if (notify == 1 && r->isConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        QObject::disconnect(r, &MediaTrackRelay::sessionEnded, nullptr, nullptr);
        if (r->moonlightShim()) r->moonlightShim()->stopConnection();
        r->stop();
        r->deleteLater();
    }
    if (g_State->streamRelay) {
        StreamRelay* r = g_State->streamRelay;
        g_State->streamRelay = nullptr;
        if (notify == 1 && r->isClientConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        QObject::disconnect(r, &StreamRelay::sessionEnded, nullptr, nullptr);
        if (r->moonlightShim()) r->moonlightShim()->stopConnection();
        r->stop();
        r->deleteLater();
    }

    if (!g_State->endedEmitted) {
        g_State->endedEmitted = true;
        emitEvent({{QStringLiteral("event"), QStringLiteral("ended")}});
    }

    // The relay graph tears down asynchronously on its own thread; give it a
    // moment before the process exits (LiStopConnection can take a while).
    QTimer::singleShot(3000, qApp, &QCoreApplication::quit);
}

} // namespace

int runStreamWorker(QCoreApplication& app)
{
    Q_UNUSED(app);
    qInfo() << "[StreamWorker] Worker process started, waiting for config on stdin";

    // ── First stdin line = session config ────────────────────────────────────
    std::string configLine;
    if (!std::getline(std::cin, configLine)) {
        qWarning() << "[StreamWorker] No config on stdin — exiting";
        return 1;
    }
    const QJsonObject cfg =
        QJsonDocument::fromJson(QByteArray::fromStdString(configLine)).object();
    if (cfg.isEmpty()) {
        qWarning() << "[StreamWorker] Invalid config JSON — exiting";
        emitEvent({{QStringLiteral("event"), QStringLiteral("response")},
                   {QStringLiteral("code"), 500},
                   {QStringLiteral("body"),
                    QJsonObject{{QStringLiteral("error"), QStringLiteral("bad worker config")}}}});
        return 1;
    }

    WorkerState state;
    g_State = &state;

    // ── Reconstruct the minimal host the session needs ───────────────────────
    // Only the fields StreamSession/NvHTTP actually read; the parent snapshots
    // them from its live NvComputer. Pairing certs come from the shared
    // IdentityManager storage (same per-user data dir as the parent).
    auto* host = new NvComputer();
    host->pairState = NvComputer::PS_PAIRED;
    host->activeAddress = NvAddress(cfg["hostAddress"].toString(),
                                    static_cast<quint16>(cfg["hostPort"].toInt(MW_HTTP_PORT)));
    host->activeHttpsPort = static_cast<quint16>(cfg["hostHttpsPort"].toInt());
    host->name = cfg["hostName"].toString();
    host->uuid = cfg["hostUuid"].toString();
    host->appVersion = cfg["appVersion"].toString();
    host->gfeVersion = cfg["gfeVersion"].toString();
    host->serverCodecModeSupport = cfg["serverCodecModeSupport"].toInt();

    auto* nam = new QNetworkAccessManager(&app);
    auto* http = new NvHTTP(nam, &app);

    // ── The /start HTTP reply is marshalled back to the parent ───────────────
    ResponseCallback respond = [](HttpResponse resp) {
        QJsonObject event;
        event["event"] = QStringLiteral("response");
        event["code"] = resp.statusCode;
        const QJsonDocument bodyDoc = QJsonDocument::fromJson(resp.body);
        if (bodyDoc.isObject())
            event["body"] = bodyDoc.object();
        else
            event["body"] = QJsonObject{{QStringLiteral("error"), QString::fromUtf8(resp.body)}};
        emitEvent(event);
    };

    // ── Build the session exactly like main.cpp's createSession ─────────────
    auto* session = new StreamSession(
        host, cfg["appId"].toInt(), http, std::move(respond),
        static_cast<quint16>(cfg["signalingPort"].toInt(48001)), cfg["serverHost"].toString(),
        static_cast<VideoCodec>(cfg["codec"].toInt()), cfg["gamingMode"].toBool(true),
        cfg["upnpEnabled"].toBool(true), cfg["internalTransport"].toString(),
        cfg["stunServer"].toString(), cfg["height"].toInt(), cfg["width"].toInt(),
        cfg["fps"].toInt(), cfg["bitrateKbps"].toInt(), cfg["yuv444"].toBool(),
        cfg["hdr"].toBool());
    session->setHttpsPort(static_cast<quint16>(cfg["serverHttpsPort"].toInt(443)));
    session->setStreamRelayPort(static_cast<quint16>(cfg["streamRelayPort"].toInt(48002)));
    session->setTransportMode(cfg["transportMode"].toString());
    session->setEnableIceTcp(cfg["iceTcp"].toBool());
    session->setLowAudio(cfg["lowAudio"].toBool());
    session->setMuteHostAudio(cfg["muteHostAudio"].toBool(true));
    session->setClientUniqueId(cfg["clientUniqueId"].toString());
    session->setIdentityIndex(cfg["identityIndex"].toInt(0));
    session->setClientIsLocal(cfg["clientIsLocal"].toBool());
    session->setAutoMode(cfg["autoMode"].toBool(true));
    session->setWsPath(cfg["wsPath"].toString(QStringLiteral("/ws")));
    if (!cfg["explicitWsUrl"].toString().isEmpty())
        session->setExplicitWsUrl(cfg["explicitWsUrl"].toString());
    if (cfg["codecOverridden"].toBool())
        session->setCodecOverridden(true, static_cast<VideoCodec>(cfg["originalCodec"].toInt()));
    {
        QStringList chain;
        for (const auto& v : cfg["transportChain"].toArray())
            chain.append(v.toString());
        session->setTransportChain(chain, cfg["transportIndex"].toInt());
    }

    // ── Relay tracking: unexpected end → "ended" event + teardown + exit ─────
    // (The parent owns the Sunshine /cancel, keyed by this session's uniqueid.)
    QObject::connect(session, &StreamSession::relayCreated, qApp, [&state](DataChannelRelay* r) {
        state.relay = r;
        QObject::connect(r, &DataChannelRelay::sessionEnded, qApp,
                         []() { teardownAndExit(0); });
    });
    QObject::connect(session, &StreamSession::mediaTrackRelayCreated, qApp,
                     [&state](MediaTrackRelay* r) {
                         state.mediaRelay = r;
                         QObject::connect(r, &MediaTrackRelay::sessionEnded, qApp,
                                          []() { teardownAndExit(0); });
                     });
    QObject::connect(session, &StreamSession::streamRelayCreated, qApp,
                     [&state](StreamRelay* r) {
                         state.streamRelay = r;
                         QObject::connect(r, &StreamRelay::sessionEnded, qApp,
                                          []() { teardownAndExit(0); });
                     });
    // Session failed before any relay existed (Sunshine rejection, bind failure):
    // the response event already carried the error — just exit.
    QObject::connect(session, &StreamSession::sessionFailed, qApp, [](const QString& err) {
        qWarning() << "[StreamWorker] Session failed:" << err;
        teardownAndExit(0);
    });

    // ── stdin command pump (blocking reads on a plain thread — portable) ─────
    std::thread stdinThread([]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QJsonObject msg =
                QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
            const QString cmd = msg["cmd"].toString();
            if (cmd == QLatin1String("quit")) {
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(0); },
                                          Qt::QueuedConnection);
            } else if (cmd == QLatin1String("takenOver")) {
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(1); },
                                          Qt::QueuedConnection);
            } else if (cmd == QLatin1String("revoked")) {
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(2); },
                                          Qt::QueuedConnection);
            }
        }
        // EOF: the parent is gone — never stream without a supervisor.
        QMetaObject::invokeMethod(qApp, []() { teardownAndExit(0); }, Qt::QueuedConnection);
    });
    stdinThread.detach();

    session->start();

    const int rc = app.exec();
    qInfo() << "[StreamWorker] Worker exiting rc=" << rc;
    return rc;
}
