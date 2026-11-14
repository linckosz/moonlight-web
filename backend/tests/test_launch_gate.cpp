/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A stream is started or ended only by the browser its session was paired with
 * (LaunchGate): /start, /quit and stop-session behind the very guards main.cpp
 * registers, driven with requests built by hand. Each route's own handler stands
 * here as a counter: a request the gate refuses never reaches it, so nothing of
 * the host is touched — no take-over, no screen brought back, no worker, no app,
 * no stream ended.
 */
#include "test_framework.h"

#include "common/PairingCrypto.h"
#include "server/RestRouter.h"
#include "server/routes/LaunchGate.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <utility>

namespace {

const QString kHostId = QStringLiteral("host-key-id");
const QString kHost = QStringLiteral("host-uuid-1");

struct Browser
{
    QByteArray pem;
    QByteArray spki;
};

Browser newBrowser()
{
    Browser b;
    b.pem = PairingCrypto::generatePrivateKeyPem();
    b.spki = PairingCrypto::publicKeySpkiFromPrivatePem(b.pem);
    return b;
}

/// How a request reaches the host.
enum class Line
{
    Socket, ///< a browser on a socket: the cookie is all it carries
    Tunnel, ///< through the rendezvous tunnel, whose line proved the key
    TvHttp, ///< a TV on plain HTTP, with a gate of its own
};

/// The server around the gate: sessions, their keys, the pool's live streams,
/// the clock, and what each guarded route's own handler was asked.
struct Server
{
    RestRouter router;
    QHash<QString, QByteArray> keys;              ///< session → its pairing key
    QHash<QString, QPair<QString, QString>> live; ///< "host|slot" → {session, stream}
    QStringList refusedFrom;
    qint64 clock = 1'000'000;
    int started = 0;
    int quits = 0;
    int stops = 0;
    LaunchGate gate;

    Server()
        : gate({[](const HttpRequest& req) { return req.headers.value(QStringLiteral("session")); },
                [this](const HttpRequest& req) {
                    if (req.viaTunnel || req.lanHttp) return QByteArray();
                    return keys.value(req.headers.value(QStringLiteral("session")));
                },
                []() { return kHostId; },
                [this](const QString& address) { refusedFrom.append(address); },
                [this](const QString& hostUuid, int slot, const QString& session) {
                    const auto it = live.constFind(hostUuid + '|' + QString::number(slot));
                    if (it == live.constEnd() || it->first != session) return QString();
                    return it->second;
                },
                [this]() { return clock; }})
    {
        gate.registerRoutes(router);
        router.postAsync(QStringLiteral("/api/hosts/:id/start"),
                         gate.guardStart([this](const HttpRequest&, ResponseCallback respond) {
                             started++;
                             respond(HttpResponse::json(QJsonObject{{"status", "streaming"}}));
                         }));
        router.postAsync(QStringLiteral("/api/hosts/:id/quit"),
                         gate.guardQuit([this](const HttpRequest&, ResponseCallback respond) {
                             quits++;
                             respond(HttpResponse::json(QJsonObject{{"status", "stopped"}}));
                         }));
        router.postAsync(QStringLiteral("/api/hosts/:id/stop-session"),
                         gate.guardStop([this](const HttpRequest&, ResponseCallback respond) {
                             stops++;
                             respond(HttpResponse::json(QJsonObject{{"status", "stopped"}}));
                         }));
    }

    HttpResponse send(const QString& route, const QJsonObject& body, const QString& session,
                      Line line = Line::Socket)
    {
        HttpRequest req;
        req.method = QStringLiteral("POST");
        req.path = QStringLiteral("/api/hosts/%1/%2").arg(kHost, route);
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        req.clientAddress = QStringLiteral("192.168.1.66");
        req.headers.insert(QStringLiteral("session"), session);
        req.viaTunnel = line == Line::Tunnel;
        req.lanHttp = line == Line::TvHttp;
        HttpResponse reply;
        reply.statusCode = 0;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }

    /// A challenge for @p session's next launch, Stop or stop-session.
    QByteArray challenge(const QString& session)
    {
        const QJsonObject obj =
            QJsonDocument::fromJson(send("launch-proof", {}, session).body).object();
        return QByteArray::fromBase64(obj.value("nonce").toString().toLatin1());
    }
};

QJsonObject jsonOf(const HttpResponse& reply)
{
    return QJsonDocument::fromJson(reply.body).object();
}

QString codeOf(const HttpResponse& reply)
{
    return jsonOf(reply).value(QStringLiteral("code")).toString();
}

/// {nonce, sig}: @p b's signature over @p fields, under @p label.
QJsonObject signedBy(const Browser& b, const char* label, const QByteArray& nonce,
                     const QList<QByteArray>& fields, const QString& hostId = kHostId)
{
    QList<QByteArray> all{QByteArray(label), hostId.toUtf8(), nonce};
    all.append(fields);
    const QByteArray sig = PairingCrypto::sign(b.pem, PairingCrypto::buildSignedMessage(all));
    return QJsonObject{{"nonce", QString::fromLatin1(nonce.toBase64())},
                       {"sig", QString::fromLatin1(sig.toBase64())}};
}

/// /start's body for @p appId, signed by @p b over @p nonce as it says, or
/// over @p signedAs when given.
QJsonObject launch(const Browser& b, const QByteArray& nonce, QJsonObject body,
                   const QJsonObject& signedAs = QJsonObject())
{
    body["launch_proof"] =
        signedBy(b, LaunchGate::kLaunchLabel, nonce,
                 LaunchGate::startFields(kHost, signedAs.isEmpty() ? body : signedAs));
    return body;
}

} // namespace

void run_launch_gate_tests()
{
    SECTION("launch gate — the signed bytes, as the page builds them");
    {
        // The same vectors are pinned in frontend/test/launchProof.test.js.
        const QByteArray n("\x01\x02\x03", 3);
        auto hex = [](const char* label, const QByteArray& nonce, const QList<QByteArray>& f) {
            QList<QByteArray> all{QByteArray(label), QByteArrayLiteral("H"), nonce};
            all.append(f);
            return QString::fromLatin1(PairingCrypto::buildSignedMessage(all).toHex());
        };
        CHECK_EQ(hex(LaunchGate::kLaunchLabel, n,
                     LaunchGate::startFields(
                         QStringLiteral("U"),
                         QJsonObject{{"appId", 7}, {"standby", true}, {"session_slot", 1}})),
                 QStringLiteral("000000114d572d42494e442d76317c6c61756e6368"
                                "0000000148"
                                "00000003010203"
                                "000000057374617274"
                                "0000000155"
                                "0000000137"
                                "0000000131"
                                "0000000131"));
        CHECK_EQ(hex(LaunchGate::kLaunchLabel, n,
                     LaunchGate::quitFields(QStringLiteral("U"), QJsonObject())),
                 QStringLiteral("000000114d572d42494e442d76317c6c61756e6368"
                                "0000000148"
                                "00000003010203"
                                "0000000471756974"
                                "0000000155"
                                "000000022d31"));
        CHECK_EQ(hex(LaunchGate::kLaunchLabel, n, LaunchGate::stopFields(QStringLiteral("U"))),
                 QStringLiteral("000000114d572d42494e442d76317c6c61756e6368"
                                "0000000148"
                                "00000003010203"
                                "0000000473746f70"
                                "0000000155"));
        CHECK_EQ(hex(LaunchGate::kQuitLabel, n,
                     LaunchGate::ticketFields(QStringLiteral("U"), 0, QStringLiteral("s1"))),
                 QStringLiteral("0000000f4d572d42494e442d76317c71756974"
                                "0000000148"
                                "00000003010203"
                                "0000000155"
                                "0000000130"
                                "000000027331"));
    }

    SECTION("launch gate — the paired browser's launch goes on, once");
    {
        Server s;
        const Browser b = newBrowser();
        s.keys["S"] = b.spki;
        const QJsonObject asked =
            QJsonDocument::fromJson(s.send("launch-proof", {}, "S").body).object();
        CHECK(asked.value("required").toBool());
        CHECK_EQ(asked.value("host_id").toString(), kHostId);
        const QByteArray nonce = QByteArray::fromBase64(asked.value("nonce").toString().toLatin1());
        CHECK_EQ(nonce.size(), 32);
        const QJsonObject body = launch(b, nonce, QJsonObject{{"appId", 3}});
        CHECK_EQ(s.send("start", body, "S").statusCode, 200);
        CHECK_EQ(s.started, 1);
        // Spent: the same request never launches again, and counts.
        const HttpResponse again = s.send("start", body, "S");
        CHECK_EQ(again.statusCode, 409);
        CHECK_EQ(codeOf(again), QStringLiteral("key_refused"));
        CHECK_EQ(s.started, 1);
        CHECK_EQ(s.refusedFrom.size(), 1);
        CHECK_EQ(s.gate.pendingProofs(), 0);
    }

    SECTION("launch gate — a session that is another browser's launches nothing");
    {
        Server s;
        const Browser b = newBrowser();
        const Browser webApp = newBrowser(); // the iOS web app: the cookie, a key of its own
        s.keys["S"] = b.spki;
        // Signed with another key.
        const HttpResponse wrong =
            s.send("start", launch(webApp, s.challenge("S"), QJsonObject{{"appId", 3}}), "S");
        CHECK_EQ(wrong.statusCode, 409);
        CHECK_EQ(codeOf(wrong), QStringLiteral("key_refused"));
        CHECK_EQ(s.refusedFrom.size(), 1);
        // Not signed at all: told so, not counted.
        const HttpResponse none = s.send("start", QJsonObject{{"appId", 3}}, "S");
        CHECK_EQ(none.statusCode, 409);
        CHECK_EQ(codeOf(none), QStringLiteral("launch_proof_required"));
        CHECK_EQ(s.refusedFrom.size(), 1);
        // A challenge of another session's.
        s.keys["T"] = newBrowser().spki;
        CHECK_EQ(
            s.send("start", launch(b, s.challenge("T"), QJsonObject{{"appId", 3}}), "S").statusCode,
            409);
        // A nonce the host never drew.
        CHECK_EQ(s.send("start",
                        launch(b, PairingCrypto::generateNonce(), QJsonObject{{"appId", 3}}), "S")
                     .statusCode,
                 409);
        // Signed for another app, slot or leg than the request asks for.
        CHECK_EQ(s.send("start",
                        launch(b, s.challenge("S"), QJsonObject{{"appId", 3}},
                               QJsonObject{{"appId", 4}}),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.send("start",
                        launch(b, s.challenge("S"), QJsonObject{{"appId", 3}, {"session_slot", 1}},
                               QJsonObject{{"appId", 3}, {"session_slot", 0}}),
                        "S")
                     .statusCode,
                 409);
        // A standby leg is a launch like any other: no proof, no leg; and a
        // proof of a primary launch does not stand for one.
        CHECK_EQ(
            s.send("start", QJsonObject{{"appId", 3}, {"standby", true}, {"session_slot", 1}}, "S")
                .statusCode,
            409);
        CHECK_EQ(s.send("start",
                        launch(b, s.challenge("S"),
                               QJsonObject{{"appId", 3}, {"standby", true}, {"session_slot", 1}},
                               QJsonObject{{"appId", 3}, {"session_slot", 1}}),
                        "S")
                     .statusCode,
                 409);
        // "This PC's sign-in screen" or "This PC's screen" with another key: the
        // route's handler, which brings the session back to the screen, is
        // never entered.
        CHECK_EQ(
            s.send("start", launch(webApp, s.challenge("S"), QJsonObject{{"appId", 999998}}), "S")
                .statusCode,
            409);
        CHECK_EQ(s.started, 0);
        // The right one still goes after all that.
        CHECK_EQ(
            s.send("start", launch(b, s.challenge("S"), QJsonObject{{"appId", 3}}), "S").statusCode,
            200);
        CHECK_EQ(s.started, 1);
    }

    SECTION("launch gate — a challenge waits thirty seconds, no longer");
    {
        Server s;
        const Browser b = newBrowser();
        s.keys["S"] = b.spki;
        const QByteArray late = s.challenge("S");
        s.clock += LaunchGate::kProofLifetimeMs;
        CHECK_EQ(s.send("start", launch(b, late, QJsonObject{{"appId", 3}}), "S").statusCode, 409);
        const QByteArray onTime = s.challenge("S");
        s.clock += LaunchGate::kProofLifetimeMs - 1;
        CHECK_EQ(s.send("start", launch(b, onTime, QJsonObject{{"appId", 3}}), "S").statusCode,
                 200);
        CHECK_EQ(s.started, 1);
    }

    SECTION("launch gate — nothing to prove: the tunnel, a TV, a session without a key");
    {
        Server s;
        s.keys["S"] = newBrowser().spki;
        CHECK_EQ(s.send("start", QJsonObject{{"appId", 3}}, "S", Line::Tunnel).statusCode, 200);
        CHECK_EQ(s.send("start", QJsonObject{{"appId", 3}}, "S", Line::TvHttp).statusCode, 200);
        CHECK_EQ(s.send("quit", QJsonObject(), "S", Line::Tunnel).statusCode, 200);
        CHECK_EQ(s.send("quit", QJsonObject(), "S", Line::TvHttp).statusCode, 200);
        // Paired before MW-BIND-v1, or never bound: as it streams today.
        CHECK(!jsonOf(s.send("launch-proof", {}, "U")).value("required").toBool(true));
        CHECK(!jsonOf(
                   s.send("quit-ticket", QJsonObject{{"session_slot", 0}, {"stream_id", "x"}}, "U"))
                   .value("required")
                   .toBool(true));
        CHECK_EQ(s.send("start", QJsonObject{{"appId", 3}}, "U").statusCode, 200);
        CHECK_EQ(s.send("stop-session", QJsonObject(), "U").statusCode, 200);
        CHECK_EQ(s.started, 3);
        CHECK_EQ(s.quits, 2);
        CHECK_EQ(s.stops, 1);
        CHECK(s.refusedFrom.isEmpty());
    }

    SECTION("launch gate — a Stop and the host's escape hatch, signed fresh");
    {
        Server s;
        const Browser b = newBrowser();
        const Browser webApp = newBrowser();
        s.keys["S"] = b.spki;
        auto quit = [&](const Browser& by, QJsonObject body, const QJsonObject& signedAs = {}) {
            body["launch_proof"] =
                signedBy(by, LaunchGate::kLaunchLabel, s.challenge("S"),
                         LaunchGate::quitFields(kHost, signedAs.isEmpty() ? body : signedAs));
            return s.send("quit", body, "S");
        };
        CHECK_EQ(quit(b, QJsonObject{{"session_slot", 0}}).statusCode, 200);
        CHECK_EQ(quit(b, QJsonObject()).statusCode, 200);
        CHECK_EQ(s.quits, 2);
        // Another key: the app and its stream keep running, and it counts.
        const HttpResponse wrong = quit(webApp, QJsonObject{{"session_slot", 0}});
        CHECK_EQ(wrong.statusCode, 409);
        CHECK_EQ(codeOf(wrong), QStringLiteral("key_refused"));
        CHECK_EQ(s.refusedFrom.size(), 1);
        // Signed for every slot, sent for one; and none at all.
        CHECK_EQ(
            quit(b, QJsonObject{{"session_slot", 0}}, QJsonObject{{"quit_app", false}}).statusCode,
            409);
        CHECK_EQ(codeOf(s.send("quit", QJsonObject{{"session_slot", 0}}, "S")),
                 QStringLiteral("launch_proof_required"));
        CHECK_EQ(s.quits, 2);

        auto stop = [&](const Browser& by) {
            return s.send("stop-session",
                          QJsonObject{{"launch_proof",
                                       signedBy(by, LaunchGate::kLaunchLabel, s.challenge("S"),
                                                LaunchGate::stopFields(kHost))}},
                          "S");
        };
        CHECK_EQ(stop(webApp).statusCode, 409);
        CHECK_EQ(s.send("stop-session", QJsonObject(), "S").statusCode, 409);
        CHECK_EQ(s.stops, 0);
        CHECK_EQ(stop(b).statusCode, 200);
        CHECK_EQ(s.stops, 1);
    }

    SECTION("launch gate — a proof stands for its own route only");
    {
        Server s;
        const Browser b = newBrowser();
        s.keys["S"] = b.spki;
        const QJsonObject launchBody{{"appId", 3}};
        const QList<QByteArray> asStart = LaunchGate::startFields(kHost, launchBody);
        const QList<QByteArray> asQuit = LaunchGate::quitFields(kHost, QJsonObject());
        const QList<QByteArray> asStop = LaunchGate::stopFields(kHost);
        auto with = [](QJsonObject body, const QJsonObject& proof) {
            body["launch_proof"] = proof;
            return body;
        };
        // A launch's proof never stops anything, a Stop's never launches.
        CHECK_EQ(s.send("quit",
                        with({}, signedBy(b, LaunchGate::kLaunchLabel, s.challenge("S"), asStart)),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.send("stop-session",
                        with({}, signedBy(b, LaunchGate::kLaunchLabel, s.challenge("S"), asStart)),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.send("start",
                        with(launchBody,
                             signedBy(b, LaunchGate::kLaunchLabel, s.challenge("S"), asQuit)),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.send("start",
                        with(launchBody,
                             signedBy(b, LaunchGate::kLaunchLabel, s.challenge("S"), asStop)),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.send("stop-session",
                        with({}, signedBy(b, LaunchGate::kLaunchLabel, s.challenge("S"), asQuit)),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.send("quit",
                        with({}, signedBy(b, LaunchGate::kLaunchLabel, s.challenge("S"), asStop)),
                        "S")
                     .statusCode,
                 409);
        // Nor under another label: the sharing board's, a quit ticket's.
        CHECK_EQ(s.send("start",
                        with(launchBody,
                             signedBy(b, "MW-BIND-v1|share-owner", s.challenge("S"), asStart)),
                        "S")
                     .statusCode,
                 409);
        CHECK_EQ(s.started + s.quits + s.stops, 0);
    }

    SECTION("launch gate — a closing page's ticket ends its own stream, once");
    {
        Server s;
        const Browser b = newBrowser();
        s.keys["S"] = b.spki;
        s.live[kHost + "|0"] = {"S", "stream-A"};
        auto ticketFor = [&](const QString& session, int slot, const QString& stream,
                             const Browser& by) {
            const HttpResponse r = s.send(
                "quit-ticket", QJsonObject{{"session_slot", slot}, {"stream_id", stream}}, session);
            const QByteArray nonce =
                QByteArray::fromBase64(jsonOf(r).value("nonce").toString().toLatin1());
            return signedBy(by, LaunchGate::kQuitLabel, nonce,
                            LaunchGate::ticketFields(kHost, slot, stream));
        };
        auto beacon = [&](const QJsonObject& ticket, int slot = 0) {
            return s.send("quit", QJsonObject{{"session_slot", slot}, {"quit_ticket", ticket}},
                          "S");
        };

        // None for a slot that does not carry that stream of this session.
        CHECK_EQ(codeOf(s.send("quit-ticket",
                               QJsonObject{{"session_slot", 0}, {"stream_id", "stream-Z"}}, "S")),
                 QStringLiteral("no_such_stream"));
        CHECK_EQ(
            s.send("quit-ticket", QJsonObject{{"session_slot", 1}, {"stream_id", "stream-A"}}, "S")
                .statusCode,
            409);
        s.keys["T"] = newBrowser().spki;
        CHECK_EQ(
            s.send("quit-ticket", QJsonObject{{"session_slot", 0}, {"stream_id", "stream-A"}}, "T")
                .statusCode,
            409);
        CHECK_EQ(s.gate.pendingTickets(), 0);

        // The beacon with its ticket ends the stream; its twin (beforeunload
        // and pagehide both fire) is refused and not counted.
        const QJsonObject t = ticketFor("S", 0, "stream-A", b);
        CHECK_EQ(beacon(t).statusCode, 200);
        CHECK_EQ(s.quits, 1);
        const HttpResponse twin = beacon(t);
        CHECK_EQ(twin.statusCode, 409);
        CHECK_EQ(codeOf(twin), QStringLiteral("key_refused"));
        CHECK_EQ(s.quits, 1);
        CHECK(s.refusedFrom.isEmpty());

        // Signed by another key: refused, and that one counts.
        CHECK_EQ(beacon(ticketFor("S", 0, "stream-A", newBrowser())).statusCode, 409);
        CHECK_EQ(s.refusedFrom.size(), 1);
        CHECK_EQ(s.quits, 1);

        // Sent for another slot than the one it was drawn for.
        s.live[kHost + "|1"] = {"S", "stream-L"};
        CHECK_EQ(beacon(ticketFor("S", 0, "stream-A", b), 1).statusCode, 409);
        CHECK_EQ(s.quits, 1);
        CHECK_EQ(s.refusedFrom.size(), 1);
    }

    SECTION("launch gate — a ticket goes with its stream");
    {
        Server s;
        const Browser b = newBrowser();
        s.keys["S"] = b.spki;
        s.keys["T"] = newBrowser().spki;
        s.live[kHost + "|0"] = {"S", "stream-A"};
        auto ticketFor = [&](const QString& stream) {
            const HttpResponse r =
                s.send("quit-ticket", QJsonObject{{"session_slot", 0}, {"stream_id", stream}}, "S");
            return signedBy(b, LaunchGate::kQuitLabel,
                            QByteArray::fromBase64(jsonOf(r).value("nonce").toString().toLatin1()),
                            LaunchGate::ticketFields(kHost, 0, stream));
        };
        auto beacon = [&](const QJsonObject& ticket, const QString& session = "S") {
            return s.send("quit", QJsonObject{{"session_slot", 0}, {"quit_ticket", ticket}},
                          session);
        };

        // Stream A relaunched as B on the same slot (a crash, a take-over): A's
        // ticket ends nothing, B goes on.
        const QJsonObject ofA = ticketFor("stream-A");
        CHECK_EQ(s.gate.pendingTickets(), 1);
        s.live[kHost + "|0"] = {"S", "stream-B"};
        CHECK_EQ(beacon(ofA).statusCode, 409);
        CHECK_EQ(s.quits, 0);
        CHECK(s.refusedFrom.isEmpty());

        // Once a stream ends, however it does, its ticket is gone.
        const QJsonObject ofB = ticketFor("stream-B");
        CHECK_EQ(s.gate.pendingTickets(), 1);
        s.gate.streamEnded("stream-B");
        CHECK_EQ(s.gate.pendingTickets(), 0);
        CHECK_EQ(beacon(ofB).statusCode, 409);
        s.live.remove(kHost + "|0");
        CHECK_EQ(beacon(ofB).statusCode, 409);
        CHECK_EQ(s.quits, 0);

        // Another session's, on its own slot or this one's.
        s.live[kHost + "|0"] = {"S", "stream-C"};
        const QJsonObject ofC = ticketFor("stream-C");
        CHECK_EQ(beacon(ofC, "T").statusCode, 409);
        CHECK_EQ(s.quits, 0);

        // A newer ticket for one stream stands for the one before it.
        const QJsonObject older = ticketFor("stream-C");
        const QJsonObject newer = ticketFor("stream-C");
        CHECK_EQ(beacon(older).statusCode, 409);
        CHECK_EQ(beacon(newer).statusCode, 200);
        CHECK_EQ(s.quits, 1);

        // Never longer than the cap, a stream forgotten or not.
        s.live[kHost + "|0"] = {"S", "stream-D"};
        const QJsonObject ofD = ticketFor("stream-D");
        s.clock += LaunchGate::kTicketLifetimeMs;
        CHECK_EQ(beacon(ofD).statusCode, 409);
        CHECK_EQ(s.quits, 1);
        CHECK(s.refusedFrom.isEmpty());
    }
}
