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
#include "CoopSessionResolver.h"
#include "../../backend/streambackend/StreamBackendRegistry.h"
#include "../../backend/streambackend/StreamBackendSetup.h"
#include "../DataChannelRelay.h"
#include "../MediaTrackRelay.h"
#include "../StreamRelay.h"
#include "../SignalingServer.h"
#include "../IMediaEngine.h"
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
    QPointer<StreamSession> session;
    QPointer<DataChannelRelay> relay;
    QPointer<MediaTrackRelay> mediaRelay;
    QPointer<StreamRelay> streamRelay;
    bool endedEmitted = false;
    bool exiting = false;
};

WorkerState* g_State = nullptr;

/// Swap the input policy under a running stream.
///
/// It goes to the RELAY, never to the session. StreamSession is ephemeral by
/// design — it hands the graph over and deletes itself the moment the
/// connection is up ("StreamSession is ephemeral — relay lives on") — so the
/// QPointer this file keeps to it is null for the entire life of every stream.
/// Dispatching through it meant the owner's demotion reached nothing at all:
/// a guest moved from full control to gamer kept their keyboard and mouse, and
/// the board said otherwise. The relay is what outlives the launch, what every
/// inbound message is checked against, and what owns the shim.
void applyPolicyToLiveGraph(const InputMsg::Policy& policy)
{
    if (!g_State) return;

    // The policy is read on the relay thread on every inbound message, so it is
    // written there too: a plain setter from the stdin pump would be a data race
    // on the hot path. StreamRelay is not a RelayBase (the legacy WSS path
    // predates it) but carries the same two accessors, so one templated lambda
    // covers all three.
    auto push = [policy](auto* relay) {
        if (!relay) return;
        QMetaObject::invokeMethod(
            relay,
            [relay, policy]() {
                relay->setInputPolicy(policy);
                IMediaEngine* engine = relay->mediaEngine();
                if (!engine) return;
                engine->setWakeNudgeAllowed(policy.keyboardMouse);
                // Release everything, then let the client's next heartbeat put
                // back whatever it still holds *and* is still allowed to hold.
                // Anything else leaves a key down on the host that nobody can
                // lift: the guest can no longer send the key-up, and the owner
                // never pressed it.
                engine->releaseHeldInputs(true);
            },
            Qt::QueuedConnection);
    };
    push(g_State->relay.data());
    push(g_State->mediaRelay.data());
    push(g_State->streamRelay.data());
}

/// Mirror of the /quit teardown in main.cpp: stop the shim FIRST (so moonlight
/// stops calling back into a relay about to be destroyed), then stop + delete
/// the relay. Exits the process once done (short grace for the relay thread).
void teardownAndExit(int notify /*0=none, 1=takenOver, 2=revoked, 3=sessionEnded*/)
{
    if (!g_State || g_State->exiting) return;
    g_State->exiting = true;

    if (g_State->relay) {
        DataChannelRelay* r = g_State->relay;
        g_State->relay = nullptr;
        if (notify == 1 && r->isConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        if (notify == 3) r->notifyClientSessionEnded();
        QObject::disconnect(r, &DataChannelRelay::sessionEnded, nullptr, nullptr);
        if (r->mediaEngine()) r->mediaEngine()->stopConnection();
        r->stop();
        r->deleteLater();
    }
    if (g_State->mediaRelay) {
        MediaTrackRelay* r = g_State->mediaRelay;
        g_State->mediaRelay = nullptr;
        if (notify == 1 && r->isConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        if (notify == 3) r->notifyClientSessionEnded();
        QObject::disconnect(r, &MediaTrackRelay::sessionEnded, nullptr, nullptr);
        if (r->mediaEngine()) r->mediaEngine()->stopConnection();
        r->stop();
        r->deleteLater();
    }
    if (g_State->streamRelay) {
        StreamRelay* r = g_State->streamRelay;
        g_State->streamRelay = nullptr;
        if (notify == 1 && r->isClientConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        if (notify == 3) r->notifyClientSessionEnded();
        QObject::disconnect(r, &StreamRelay::sessionEnded, nullptr, nullptr);
        if (r->mediaEngine()) r->mediaEngine()->stopConnection();
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
    const QJsonObject cfg = QJsonDocument::fromJson(QByteArray::fromStdString(configLine)).object();
    if (cfg.isEmpty()) {
        qWarning() << "[StreamWorker] Invalid config JSON — exiting";
        emitEvent({{QStringLiteral("event"), QStringLiteral("response")},
                   {QStringLiteral("code"), 500},
                   {QStringLiteral("body"),
                    QJsonObject{{QStringLiteral("error"), QStringLiteral("bad worker config")}}}});
        return 1;
    }

    // Automatic storage is required: the relay callbacks below capture `state`
    // by reference, and a lambda can only capture a variable with automatic
    // storage duration. It lives on this frame for the whole event loop —
    // g_State is only dereferenced from teardown callbacks that run inside
    // app.exec() below, well before the frame unwinds — so the pointer never
    // actually dangles.
    WorkerState state;
    // cppcheck-suppress danglingLifetime
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
    // The parent's verdict on the host's OS: it holds the probe results and the
    // address list this is worked out from, and we hold none of them. Absent or
    // "unknown" leaves the scroll path on its safe default (see HostOsProbe.h).
    host->declaredOs = HostOsProbe::fromString(cfg["hostOs"].toString());
    // Without these a backend's readiness check rejects the host as unpaired.
    host->pairState = NvComputer::pairStateFromString(cfg["hostPairState"].toString());
    host->serverCertPem = cfg["hostServerCert"].toString().toUtf8();

    auto* nam = new QNetworkAccessManager(&app);
    auto* http = new NvHTTP(nam, &app);

    // This process has no ComputerManager, so it registers the providers itself
    // with a lookup that only ever knows the one host it was handed, and no
    // pairing commit — a worker has no host list to write back to.
    StreamBackendSetup::registerAll(http, nam, [host](const QString&) { return host; }, {});

    QJsonObject backendConfig;
    backendConfig[QStringLiteral("hostUuid")] = host->uuid;
    backendConfig[QStringLiteral("apiUrl")] = cfg["backendApiUrl"].toString();
    backendConfig[QStringLiteral("apiToken")] = cfg["backendApiToken"].toString();
    // What a MultiSeat seat needs to pair itself: its Apollo web UI is the only
    // way a PIN reaches it, and the control API cannot pair on our behalf.
    backendConfig[QStringLiteral("pairUser")] = cfg["backendPairUser"].toString();
    backendConfig[QStringLiteral("pairPassword")] = cfg["backendPairPassword"].toString();

    QString backendType = cfg["backendType"].toString();
    if (backendType.isEmpty()) backendType = QStringLiteral("gamestream");
    std::shared_ptr<IStreamBackend> backend(
        StreamBackendRegistry::instance().create(backendType, backendConfig).release());

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
    session->setMediaPort(static_cast<quint16>(cfg["mediaPort"].toInt(48010)));
    session->setTransportMode(cfg["transportMode"].toString());
    session->setEnableIceTcp(cfg["iceTcp"].toBool());
    session->setLowAudio(cfg["lowAudio"].toBool());
    session->setMuteHostAudio(cfg["muteHostAudio"].toBool(true));
    // Absent (an older parent) → off, which is exactly today's behaviour.
    session->setRideOutLoss(cfg["rideOutLoss"].toBool(false));
    session->setClientPresentation(cfg["clientRefreshMilliHz"].toInt(0),
                                   cfg["clientVsync"].toBool(false));
    session->setBackend(backend);
    session->setClientUniqueId(cfg["clientUniqueId"].toString());
    session->setPreferResume(cfg["preferResume"].toBool(false));
    // Absent (an older parent) → Public: the conservative reading, since every
    // non-Public verdict grants something.
    session->setClientKind(NetClassify::fromString(cfg["clientKind"].toString()));
    // MW-BIND-v1 material, arriving on stdin rather than the command line so the
    // host private key never shows up in the process list.
    session->setPairingIdentity(
        cfg["mwBindHostId"].toString(),
        QByteArray::fromBase64(cfg["mwBindHostKey"].toString().toUtf8()),
        QByteArray::fromBase64(cfg["mwBindBrowserKey"].toString().toUtf8()));
    session->setAutoMode(cfg["autoMode"].toBool(true));
    session->setWsPath(cfg["wsPath"].toString(QStringLiteral("/ws")));
    // A shared session (invited player) arrives with the permissions the owner
    // ticked. Absent = the owner's own session: unrestricted. This is only the
    // starting value: the owner can move it from the sharing board at any time,
    // and it arrives here as a "setPolicy" command on stdin.
    if (cfg.contains(QStringLiteral("inputPolicy"))) {
        const QJsonObject pol = cfg["inputPolicy"].toObject();
        InputMsg::Policy policy;
        policy.gamepad = pol["gamepad"].toBool(false);
        policy.keyboardMouse = pol["keyboardMouse"].toBool(false);
        session->setInputPolicy(policy);
        qInfo() << "[StreamWorker] Input policy: gamepad=" << policy.gamepad
                << "keyboardMouse=" << policy.keyboardMouse;
    }
    // What the media path revealed about the host's OS. Only the parent keeps a
    // host list, so this is the only way the observation outlives the stream —
    // and outliving it is the point: without it every session would spend its
    // first seconds quantizing scroll on a host already known not to need it.
    QObject::connect(session, &StreamSession::hostIpTtlObserved, session, [](int ttl) {
        emitEvent(
            {{QStringLiteral("event"), QStringLiteral("hostIpTtl")}, {QStringLiteral("ttl"), ttl}});
    });

    session->setGamepadOffset(cfg["gamepadOffset"].toInt(0));
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

    // ── Native co-op: recover the host-side session id, for reaping ──────────
    // Nothing happens here unless the backend has lobbies (Wolf). Resolving the
    // id is what lets the supervisor close the session host-side when the stream
    // ends — the worker cannot be trusted to, since the case that leaks is
    // exactly the one where it died without a teardown. Joining a shared lobby
    // so a guest sees the owner's screen is the host's own streamed UI's job
    // now (Wolf UI), not ours — see docs/integration-multiseat-wolf.md.
    {
        auto* resolver = new CoopSessionResolver(
            backend,
            [](const QString& sessionId) {
                emitEvent({{QStringLiteral("event"), QStringLiteral("coopSession")},
                           {QStringLiteral("sessionId"), sessionId}});
            },
            qApp);
        QObject::connect(session, &StreamSession::sessionStarted, qApp,
                         [resolver, session]() { resolver->begin(session->launchKey()); });
    }

    // Kept so the stdin pump can reach the live graph: a policy change from the
    // owner's board lands on the session, which pushes it to whichever relay
    // exists and releases what the old policy had held down.
    state.session = session;

    // ── Relay tracking: unexpected end → "ended" event + teardown + exit ─────
    // (The parent owns the Sunshine /cancel, keyed by this session's uniqueid.)
    QObject::connect(session, &StreamSession::relayCreated, qApp, [&state](DataChannelRelay* r) {
        state.relay = r;
        QObject::connect(r, &DataChannelRelay::sessionEnded, qApp, []() { teardownAndExit(0); });
    });
    QObject::connect(
        session, &StreamSession::mediaTrackRelayCreated, qApp, [&state](MediaTrackRelay* r) {
            state.mediaRelay = r;
            QObject::connect(r, &MediaTrackRelay::sessionEnded, qApp, []() { teardownAndExit(0); });
        });
    QObject::connect(session, &StreamSession::streamRelayCreated, qApp, [&state](StreamRelay* r) {
        state.streamRelay = r;
        QObject::connect(r, &StreamRelay::sessionEnded, qApp, []() { teardownAndExit(0); });
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
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(0); }, Qt::QueuedConnection);
            } else if (cmd == QLatin1String("takenOver")) {
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(1); }, Qt::QueuedConnection);
            } else if (cmd == QLatin1String("revoked")) {
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(2); }, Qt::QueuedConnection);
            } else if (cmd == QLatin1String("sessionEnded")) {
                QMetaObject::invokeMethod(qApp, []() { teardownAndExit(3); }, Qt::QueuedConnection);
            } else if (cmd == QLatin1String("setPolicy")) {
                // The owner moved this player's inputs while they were playing.
                // Not a teardown: the stream carries on under the new rules.
                InputMsg::Policy policy;
                policy.gamepad = msg["gamepad"].toBool(false);
                policy.keyboardMouse = msg["keyboardMouse"].toBool(false);
                qInfo() << "[StreamWorker] Input policy now: gamepad=" << policy.gamepad
                        << "keyboardMouse=" << policy.keyboardMouse;
                QMetaObject::invokeMethod(
                    qApp,
                    [policy]() {
                        // Both, and in this order. The session only still
                        // exists in the sliver before the connection comes up,
                        // where it is the thing that will build the relay and
                        // has to be told; after that it is gone and the relay
                        // is the only thing left to tell.
                        if (g_State && g_State->session) g_State->session->applyInputPolicy(policy);
                        applyPolicyToLiveGraph(policy);
                    },
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
