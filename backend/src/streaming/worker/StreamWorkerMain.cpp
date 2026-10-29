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
#include "../InputMessageCodec.h"
#include "../../server/AppSettings.h"
#include "mw/native/FeedArbiter.h"
#include "mw/native/NativeHost.h"
#include "CoopSessionResolver.h"
#include "../../backend/streambackend/StreamBackendRegistry.h"
#include "../../backend/streambackend/StreamBackendSetup.h"
#include "../DataChannelRelay.h"
#include "../FeedPublisher.h"
#include "../NativeMediaEngine.h"
#include "../../backend/streambackend/NativeHostBackend.h"
#include "../MediaTrackRelay.h"
#include "../StreamRelay.h"
#include "../SignalingServer.h"
#include "../IMediaEngine.h"
#include "../ConsoleSession.h"
#include "../HidParkingTransfer.h"
#include "../PacketMarking.h"
#include "../WorkerService.h"
#include "../../backend/NvComputer.h"
#include "../../backend/NvHTTP.h"
#include "../../backend/IdentityManager.h"
#include "../../common/Types.h"
#include "../../common/RunFlags.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QDebug>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(__linux__)
#include <cerrno>
#include <unistd.h>
#endif

namespace {

/// Write one protocol event line on stdout (the only stdout writer in worker
/// mode — all logging goes to the log file / stderr).
void emitEvent(const QJsonObject& event)
{
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n";
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

/// One line from the server on stdin, without its newline; false at the end.
///
/// Not std::getline on std::cin under Linux: the glibc of Ubuntu 24.04 (2.39)
/// takes every stdio stream's lock in exit() to flush it — 22.04's (2.35) did
/// not — and std::cin reads through stdin's FILE, whose lock it holds for as
/// long as a read blocks, which the command pump's does until the server writes
/// again. Each worker's last return then hung in exit() until the server killed
/// it: exit code 9 at the end of every stream. read(2) on the descriptor holds
/// no lock. Called from one thread at a time: the config line, then the pump.
bool readServerLine(std::string& line)
{
#if defined(__linux__)
    static std::string pending;
    for (;;) {
        const size_t end = pending.find('\n');
        if (end != std::string::npos) {
            line.assign(pending, 0, end);
            pending.erase(0, end + 1);
            return true;
        }
        char buffer[4096];
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (got > 0) {
            pending.append(buffer, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        // The end, or a broken pipe: what came before it is still a line.
        if (pending.empty()) return false;
        line.swap(pending);
        pending.clear();
        return true;
    }
#else
    return static_cast<bool>(std::getline(std::cin, line));
#endif
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

/// Mouse Keys the native engine turned on, and what to put back — or let go —
/// sent to the server as it happens: a worker that is killed holding them
/// cannot put them back, the parent can. Straight from the engine, which lives
/// as long as the stream: the StreamSession goes once the stream is up, and
/// with it anything relayed through it (bench of 16/10: the end of every
/// stream went unsaid). Direct, from the capture thread or whichever stops the
/// engine: one line per write, under the stream's own lock.
void relayMouseKeys(IMediaEngine* engine)
{
    auto* native = qobject_cast<NativeMediaEngine*>(engine);
    if (!native) return;
    QObject::connect(
        native, &NativeMediaEngine::mouseKeysChanged, qApp,
        [](bool on, const QJsonObject& saved) {
            QJsonObject event{{QStringLiteral("event"), QStringLiteral("mouseKeys")},
                              {QStringLiteral("on"), on}};
            if (on) event.insert(QStringLiteral("saved"), saved);
            emitEvent(event);
        },
        Qt::DirectConnection);
}

/// Mirror of the /quit teardown in main.cpp: stop the shim FIRST (so moonlight
/// stops calling back into a relay about to be destroyed), then stop + delete
/// the relay. Exits the process once done (short grace for the relay thread).
void teardownAndExit(int notify /*0=none, 1=takenOver, 2=revoked, 3=sessionEnded*/)
{
    if (!g_State || g_State->exiting) return;
    g_State->exiting = true;

    QJsonObject hold;

    if (g_State->relay) {
        DataChannelRelay* r = g_State->relay;
        g_State->relay = nullptr;
        if (notify == 1 && r->isConnected()) r->notifyClientTakenOver();
        if (notify == 2) r->notifyClientRevoked();
        if (notify == 3) r->notifyClientSessionEnded();
        QObject::disconnect(r, &DataChannelRelay::sessionEnded, nullptr, nullptr);
        if (r->mediaEngine()) {
            r->mediaEngine()->stopConnection();
            hold = r->mediaEngine()->captureHoldReport();
        }
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
        if (r->mediaEngine()) {
            r->mediaEngine()->stopConnection();
            hold = r->mediaEngine()->captureHoldReport();
        }
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
        if (r->mediaEngine()) {
            r->mediaEngine()->stopConnection();
            hold = r->mediaEngine()->captureHoldReport();
        }
        r->stop();
        r->deleteLater();
    }

    // The wheels the engine left at rest (stopConnection parks them) go to the
    // server, which keeps them for the next stream: a game left running keeps
    // its wheel and the force feedback it set up (HidParkingTransfer). At once,
    // before "ended": a stream started again within the grace below would
    // otherwise find nothing kept and make a second wheel beside the game's
    // (bench of 11/10: ACC running, the next stream's picture froze). The pid
    // was checked when the pipes were, and is checked again: the server may
    // be gone, and its pid someone else's.
    const auto shipKept = []() {
        const quint32 serverPid = ConsoleSession::workerServerPid();
        if (!serverPid ||
            !ConsoleSession::isThisExecutable(ConsoleSession::processImage(serverPid)))
            return;
        const QJsonObject parked = HidParkingTransfer::shipToServer(serverPid);
        if (!parked.isEmpty()) emitEvent(parked);
    };
    shipKept();

    // The capture's word on the display it held (CaptureHold.h), for the
    // server's `vddmatch`: the parent alone keeps a settings file. Before
    // "ended", which the server tears the slot down on.
    if (!hold.isEmpty()) {
        hold.insert(QStringLiteral("event"), QStringLiteral("captureHold"));
        emitEvent(hold);
    }

    if (!g_State->endedEmitted) {
        g_State->endedEmitted = true;
        emitEvent({{QStringLiteral("event"), QStringLiteral("ended")}});
    }

    // The relay graph tears down asynchronously on its own thread; give it a
    // moment before the process exits (LiStopConnection can take a while).
    // Whatever an engine parked on its way down meanwhile goes too.
    QTimer::singleShot(3000, qApp, [shipKept]() {
        shipKept();
        QCoreApplication::quit();
    });
}

/// The guests' shared feed (plan « flux commun des invités »): a worker with no
/// browser of its own. It captures the owner's display once, encodes it once
/// for every guest of this native host, and publishes each frame on a local
/// pipe their workers subscribe to. No session, no relay, no signaling: the
/// engine and the pipe, and the server's commands on stdin.
int runFeedWorker(const QJsonObject& cfg)
{
    const int appId = cfg["appId"].toInt();
    QString why;
    const int displayId = NativeHostBackend::displayForKey(cfg["displayKey"].toString(), &why);
    if (displayId < 0) {
        qWarning() << "[StreamWorker] feed: no display for app" << appId << ":" << why;
        emitEvent({{QStringLiteral("event"), QStringLiteral("response")},
                   {QStringLiteral("code"), 404},
                   {QStringLiteral("body"), QJsonObject{{QStringLiteral("error"), why}}}});
        return 1;
    }

    auto* publisher =
        new FeedPublisher(cfg["feedPipe"].toString(), cfg["feedToken"].toString().toLatin1(), qApp);
    if (!publisher->start(&why)) {
        emitEvent({{QStringLiteral("event"), QStringLiteral("response")},
                   {QStringLiteral("code"), 500},
                   {QStringLiteral("body"),
                    QJsonObject{{QStringLiteral("error"), QStringLiteral("feed pipe: ") + why}}}});
        return 1;
    }

    auto* engine = new NativeMediaEngine(qApp);
    auto ended = std::make_shared<bool>(false);
    auto finish = [engine, publisher, ended]() {
        if (*ended) return;
        *ended = true;
        engine->stopConnection();
        publisher->stop();
        emitEvent({{QStringLiteral("event"), QStringLiteral("ended")}});
        QTimer::singleShot(500, qApp, &QCoreApplication::quit);
    };

    // What the guests ask of the one encoder they share, arbitrated (S6):
    // their keyframes gathered and rationed, their link reports folded into
    // the slowest one's for the feed's governor, and the host's pointer held
    // on the display only for a guest who sees it nowhere else. See FeedArbiter.
    auto arbiter = std::make_shared<mw::native::FeedArbiter>();
    auto steadyMs = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    QObject::connect(
        publisher, &FeedPublisher::controlReceived, qApp,
        [arbiter, steadyMs](int id, const QJsonObject& msg) {
            const QString type = msg.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("idr")) {
                arbiter->requestKeyframe(id, steadyMs());
            } else if (type == QLatin1String("link")) {
                mw::native::LinkFeedback fb;
                fb.owdRiseMs = msg.value(QStringLiteral("owdRiseMs")).toInt();
                fb.gaps = msg.value(QStringLiteral("gaps")).toInt();
                fb.evictions = msg.value(QStringLiteral("evictions")).toInt();
                fb.receivedFps = msg.value(QStringLiteral("fps")).toInt();
                fb.resumed = msg.value(QStringLiteral("resumed")).toBool();
                arbiter->report(id, fb, steadyMs());
            }
        },
        Qt::QueuedConnection);
    auto* arbitrate = new QTimer(qApp);
    arbitrate->setInterval(25);
    QObject::connect(arbitrate, &QTimer::timeout, qApp, [arbiter, engine, steadyMs]() {
        const int64_t now = steadyMs();
        if (arbiter->keyframeDue(now)) engine->requestIdrFrame();
        mw::native::LinkFeedback merged;
        if (arbiter->linkDue(now, merged)) engine->reportLink(merged);
    });
    arbitrate->start();
    QObject::connect(publisher, &FeedPublisher::subscriberJoined, qApp, [](int id, int slot) {
        qInfo() << "[StreamWorker] feed: guest of slot" << slot << "in (" << id << ")";
    });
    QObject::connect(publisher, &FeedPublisher::subscriberLeft, qApp, [arbiter](int id, int slot) {
        arbiter->leave(id);
        qInfo() << "[StreamWorker] feed: guest of slot" << slot << "gone (" << id
                << ") — keyframes asked" << arbiter->asked() << ", served" << arbiter->served();
    });

    QObject::connect(engine, &IMediaEngine::connectionStarted, qApp, [engine]() {
        mw::native::SessionInfo info;
        engine->sessionInfo(info);
        emitEvent({{QStringLiteral("event"), QStringLiteral("response")},
                   {QStringLiteral("code"), 200},
                   {QStringLiteral("body"),
                    QJsonObject{{QStringLiteral("status"), QStringLiteral("streaming")},
                                {QStringLiteral("feed"), engine->describeSession()},
                                {QStringLiteral("stream_width"), info.width},
                                {QStringLiteral("stream_height"), info.height},
                                {QStringLiteral("intra_refresh"), info.intraRefresh}}}});
    });
    QObject::connect(engine, &IMediaEngine::connectionFailed, qApp, [finish](const QString& err) {
        qWarning() << "[StreamWorker] feed: the engine did not start:" << err;
        emitEvent({{QStringLiteral("event"), QStringLiteral("response")},
                   {QStringLiteral("code"), 502},
                   {QStringLiteral("body"), QJsonObject{{QStringLiteral("error"), err}}}});
        finish();
    });
    QObject::connect(engine, &IMediaEngine::connectionTerminated, qApp, [finish](int) {
        qWarning() << "[StreamWorker] feed: the session ended on its own";
        finish();
    });

    // Every guest's session is carried by its own worker; this one only
    // encodes: fixed cadence, no audio, and a rate the slowest guest pulls
    // down to 60 % of the setting at most. Intra-refresh where the route the
    // machine picks for this GPU grants it, as the owner's own stream asks it
    // — never a route of its own for it: on an Arc, the D3D11 oneVPL one beside
    // the owner's D3D12 Video Encode took the owner's p99 from 7.8 to 27 ms
    // with a single guest (S9, 01/10/2026). Where it is not granted, the
    // guests' keyframes are grouped and rationed (FeedArbiter).
    NativeMediaEngine::StartParams p;
    p.displayId = displayId;
    p.width = cfg["width"].toInt();
    p.height = cfg["height"].toInt();
    p.fps = cfg["fps"].toInt(60);
    p.maxFps = p.fps;
    p.bitrateKbps = cfg["bitrateKbps"].toInt(10000);
    // HEVC with H.264 behind it, or H.264 alone once a guest has said it
    // decodes nothing else (VIDEO_FORMAT_* masks, as the owner's /start).
    p.clientVideoFormats = cfg["h264"].toBool() ? 0x000F : (0x0F00 | 0x000F);
    p.intraRefresh = true;
    p.governorFloorPercent = cfg["governorFloorPercent"].toInt(60);
    p.followDisplayShape = true;
    p.viewerAdmin = false;
    p.muteHostAudio = false;
    p.captureAudio = false;
    p.portalRestoreToken = cfg["portalRestoreToken"].toString();
    mw::native::parseVideoPipeline(cfg["nativeVideoPipeline"].toString().toStdString(),
                                   p.videoPipeline);
    p.tuningSpec = cfg["nativeTuning"].toString();
    p.feedPublisher = publisher;
    qInfo() << "[StreamWorker] feed: display" << displayId << "at" << p.width << "x" << p.height
            << "@" << p.fps << "," << p.bitrateKbps << "kbps,"
            << (cfg["h264"].toBool() ? "H.264" : "HEVC") << "— pipe" << publisher->name();

    std::thread stdinThread([finish, publisher]() {
        std::string line;
        while (readServerLine(line)) {
            const QJsonObject msg =
                QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
            const QString cmd = msg["cmd"].toString();
            if (cmd == QLatin1String("quit") || cmd == QLatin1String("sessionEnded")) {
                QMetaObject::invokeMethod(qApp, finish, Qt::QueuedConnection);
            } else if (cmd == QLatin1String("codec")) {
                // A guest arrived whose browser decodes no HEVC: the feed goes
                // H.264 for everyone, from a new worker. This one's guests are
                // told, and their pages come back the way a codec fallback
                // does; the pipe closes a moment after, once they have read it.
                const QString codec = msg["codec"].toString();
                QMetaObject::invokeMethod(
                    qApp,
                    [publisher, codec, finish]() {
                        publisher->publishControl(
                            QJsonObject{{QStringLiteral("type"), QStringLiteral("codec")},
                                        {QStringLiteral("codec"), codec}});
                        QTimer::singleShot(1000, qApp, finish);
                    },
                    Qt::QueuedConnection);
            }
        }
        // EOF: the server is gone — never encode without a supervisor.
        QMetaObject::invokeMethod(qApp, finish, Qt::QueuedConnection);
    });
    stdinThread.detach();

    engine->startCapture(p);
    const int rc = qApp->exec();
    qInfo() << "[StreamWorker] feed exiting rc=" << rc;
    return rc;
}

} // namespace

int runStreamWorker(QCoreApplication& app)
{
    Q_UNUSED(app);
    qInfo() << "[StreamWorker] Worker process started, waiting for config on stdin";

    // Ctrl+Alt+Suppr: Windows takes it from a service in session 0 only, and
    // this worker runs in the console session, SYSTEM as it may be. The
    // launcher service it came from presses it on its behalf (design §31.7).
    mw::native::NativeHost::setSecureAttentionSender([](std::string& error) {
        QString why;
        if (WorkerService::requestSecureAttention(&why)) return true;
        error = "the launcher service did not press it: " + why.toStdString();
        return false;
    });

    // ── First stdin line = session config ────────────────────────────────────
    std::string configLine;
    if (!readServerLine(configLine)) {
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

    // The server's --config, before the first AppSettings below: this process
    // reads the same file the server does, whichever way it was started.
    const QString settingsFile = cfg[QStringLiteral("settingsFile")].toString();
    if (!settingsFile.isEmpty()) AppSettings::setFileOverride(settingsFile);
    // The server's verbose mode, before the session builds its first
    // PeerConnection: libdatachannel reads its log level as each one starts.
    if (cfg[QStringLiteral("verbose")].toBool()) {
        mw::run::setVerboseFromCli(true);
        mw::run::applyVerboseLogging();
    }

    // Keyboard diagnostics, armed HERE and not only in main(): a keystroke is
    // handled by the relays this process builds, and main() sets the flag well
    // after `return runStreamWorker(app)` — so in worker mode, which is the
    // default, the switch was on in the one process that never sees a key.
    // Same trap, same shape as the latency flag: anything an input or capture
    // path reads has to be read again on this side of the fork.
    // From the server's config too: a SYSTEM worker's own AppSettings reads
    // SYSTEM's profile, where nobody wrote it.
    if (AppSettings().keyboardDebug() || cfg[QStringLiteral("keyboardDebug")].toBool(false)) {
        InputMsg::setDebug(true);
        InputMsg::setQuietProbe(&mw::native::NativeHost::secureDesktopHasInput);
        mw::native::NativeHost::setKeyboardDiagnostics(true);
        qInfo() << "[KBD] keyboard diagnostics on in the worker (never on the secure desktop)";
    }

    // The packets' DSCP, from the server's config for the same reason, before
    // the first peer connection reads it. Absent (an older parent) → this
    // process reads its own file and environment, as before.
    {
        PacketMarking::Handover marking;
        if (cfg.contains(QStringLiteral("packetMarking")))
            marking.marking = cfg[QStringLiteral("packetMarking")].toBool(true);
        if (cfg.contains(QStringLiteral("dscp")))
            marking.dscp = cfg[QStringLiteral("dscp")].toString().toStdString();
        PacketMarking::setHandover(std::move(marking));
    }

    // The guests' shared feed: an engine and a pipe, nothing a browser meets.
    if (cfg[QStringLiteral("role")].toString() == QLatin1String("feed")) return runFeedWorker(cfg);

    // The wheels the server kept from the last stream, first of all: until
    // this process answers for them, the game's force feedback requests wait.
    // The server learns at once, and lets go of its copies.
    if (cfg.contains(QStringLiteral("hidParked"))) {
        const int taken = HidParkingTransfer::adoptFromServer(
            cfg[QStringLiteral("hidParked")].toObject(), ConsoleSession::workerServerPid());
        emitEvent({{QStringLiteral("event"), QStringLiteral("hidAdopted")},
                   {QStringLiteral("count"), taken}});
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
        cfg["internalTransport"].toString(), cfg["stunServer"].toString(), cfg["height"].toInt(),
        cfg["width"].toInt(), cfg["fps"].toInt(), cfg["bitrateKbps"].toInt(),
        cfg["yuv444"].toBool(), cfg["hdr"].toBool());
    session->setHttpsPort(static_cast<quint16>(cfg["serverHttpsPort"].toInt(443)));
    session->setStreamRelayPort(static_cast<quint16>(cfg["streamRelayPort"].toInt(48002)));
    session->setMediaPort(static_cast<quint16>(cfg["mediaPort"].toInt(48550)));
    // The router hole the parent claimed for this slot, if any. Absent (no
    // gateway, UPnP off, a guest) → STUN only, as before.
    session->setUpnpMapping(cfg["upnpPublicIp"].toString(),
                            static_cast<quint16>(cfg["upnpExternalPort"].toInt(0)));
    session->setTransportMode(cfg["transportMode"].toString());
    session->setEnableIceTcp(cfg["iceTcp"].toBool());
    session->setIceTcpPassive(cfg["iceTcpPassive"].toBool(true));
    session->setDebugIceDropUdp(cfg["debugIceDropUdp"].toBool(false));
    session->setLowAudio(cfg["lowAudio"].toBool());
    session->setMuteHostAudio(cfg["muteHostAudio"].toBool(true));
    // Absent (an older parent) → off, which is exactly today's behaviour.
    session->setRideOutLoss(cfg["rideOutLoss"].toBool(false));
    // Absent (an older parent, a guest) → the engine's own repairs.
    session->setRefInvalidation(cfg["refInvalidation"].toBool(true));
    // Absent (an older parent, a guest) → no cropping, the frame on the grid.
    session->setCropsToFrame(cfg["cropsToFrame"].toBool(false));
    // A native display card: its display, as the server resolved it. This
    // process, SYSTEM's maybe, does not read the file the cards are numbered in.
    if (cfg.contains(QStringLiteral("displayKey")))
        session->setDisplayKey(cfg[QStringLiteral("displayKey")].toString());
    // One of the owner's apps: whether it is still there, for the same reason.
    if (cfg.contains(QStringLiteral("hostAppKnown")))
        session->setHostAppKnown(cfg[QStringLiteral("hostAppKnown")].toBool());
    // On our virtual display: the display it took the primary role from.
    if (cfg.contains(QStringLiteral("previousPrimary")))
        session->setPreviousPrimary(cfg[QStringLiteral("previousPrimary")].toString());
    // Absent (an older parent) → not an administrator: the safe reading,
    // since the only thing it grants is the host's administrator windows.
    session->setViewerAdmin(cfg["viewerAdmin"].toBool(false));
    // Absent (an older parent) → this process reads the file, as before.
    session->setKeyboardLayoutFidelity(cfg["keyboardLayoutFidelity"].toString());
    // Absent (an older parent) → this process reads the file, as before.
    if (cfg.contains(QStringLiteral("keyboardHostLayout")))
        session->setKeyboardHostLayout(cfg[QStringLiteral("keyboardHostLayout")].toBool());
    // Absent (an older parent, or a machine that never met a portal) → empty,
    // and the user is asked. Never a failure: a dialog is the fallback.
    session->setPortalRestoreToken(cfg["portalRestoreToken"].toString());
    // Absent (an older parent, or a guest) → the monitor stays where it is.
    session->setVirtualPrimary(cfg["virtualPrimary"].toBool(false));
    session->setGamescopeApp(cfg["gamescopeApp"].toString(), cfg["gamescopeCommand"].toString());
    session->setNativeVideoPipeline(cfg["nativeVideoPipeline"].toString());
    // Absent (an older parent, or no knob) → empty: the engine's own settings.
    session->setNativeTuning(cfg["nativeTuning"].toString());
    session->setRtpVideo(cfg["rtpVideo"].toString());
    // A guest on the guests' shared feed: absent → this session captures.
    session->setSharedFeed(cfg["feedPipe"].toString(), cfg["feedToken"].toString().toLatin1(),
                           cfg["feedSlot"].toInt(-1));
    session->setClientPresentation(cfg["clientRefreshMilliHz"].toInt(0),
                                   cfg["clientVsync"].toBool(false),
                                   cfg["clientTearingReal"].toBool(false));
    // Absent (an older parent, or a rate the viewer named) → no ceiling, and
    // the cadence keeps every freedom it had.
    session->setMaxFps(cfg["fpsCeiling"].toInt(0));
    // Absent (an older parent) → the frame keeps its size, today's behaviour.
    session->setFollowDisplayShape(cfg["followDisplayShape"].toBool(false));
    // Absent (an older parent) → the height is kept, today's behaviour.
    session->setFrameFit(cfg["fitRequestedBox"].toBool(false), cfg["allowUpscale"].toBool(false),
                         cfg["matchClientDisplay"].toBool(false), cfg["fallbackWidth"].toInt(0),
                         cfg["fallbackHeight"].toInt(0));
    session->setBackend(backend);
    session->setClientUniqueId(cfg["clientUniqueId"].toString());
    session->setPreferResume(cfg["preferResume"].toBool(false));
    // Absent (an older parent) → Public: the conservative reading, since every
    // non-Public verdict grants something.
    session->setClientKind(NetClassify::fromString(cfg["clientKind"].toString()));
    // Absent (an older parent) → false: the LAN candidate stays for private peers.
    session->setClientSameNetwork(cfg["clientSameNetwork"].toBool(false));
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
        policy.hid = pol["hid"].toBool(false);
        session->setInputPolicy(policy);
        qInfo() << "[StreamWorker] Input policy: gamepad=" << policy.gamepad
                << "keyboardMouse=" << policy.keyboardMouse << "hid=" << policy.hid;
        // A guest let into the host's administrator windows by the owner's
        // settings file, said once: read afterwards, the log shows why this
        // invited player could drive one.
        if (cfg["viewerAdmin"].toBool(false))
            qInfo() << "[StreamWorker] administrator windows open to this invited player"
                       " (share_guests_admin_windows)";
    }
    // What the media path revealed about the host's OS. Only the parent keeps a
    // host list, so this is the only way the observation outlives the stream —
    // and outliving it is the point: without it every session would spend its
    // first seconds quantizing scroll on a host already known not to need it.
    QObject::connect(session, &StreamSession::hostIpTtlObserved, session, [](int ttl) {
        emitEvent(
            {{QStringLiteral("event"), QStringLiteral("hostIpTtl")}, {QStringLiteral("ttl"), ttl}});
    });
    // The desktop portal's consent, on its way to the settings file — which
    // only the parent has. Sent as it arrives rather than at the end of the
    // session: a worker that is killed still leaves the user's click banked.
    QObject::connect(session, &StreamSession::portalGrantReceived, session,
                     [](const QString& token) {
                         emitEvent({{QStringLiteral("event"), QStringLiteral("portalGrant")},
                                    {QStringLiteral("token"), token}});
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
        relayMouseKeys(r->mediaEngine());
    });
    QObject::connect(
        session, &StreamSession::mediaTrackRelayCreated, qApp, [&state](MediaTrackRelay* r) {
            state.mediaRelay = r;
            QObject::connect(r, &MediaTrackRelay::sessionEnded, qApp, []() { teardownAndExit(0); });
            relayMouseKeys(r->mediaEngine());
        });
    QObject::connect(session, &StreamSession::streamRelayCreated, qApp, [&state](StreamRelay* r) {
        state.streamRelay = r;
        QObject::connect(r, &StreamRelay::sessionEnded, qApp, []() { teardownAndExit(0); });
        relayMouseKeys(r->mediaEngine());
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
        while (readServerLine(line)) {
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
                policy.hid = msg["hid"].toBool(false);
                qInfo() << "[StreamWorker] Input policy now: gamepad=" << policy.gamepad
                        << "keyboardMouse=" << policy.keyboardMouse << "hid=" << policy.hid;
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
