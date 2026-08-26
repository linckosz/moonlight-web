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

#include "ControlTunnel.h"

#include "server/TunnelFrame.h"

#include "common/Logger.h"
#include "common/PairingCrypto.h"
#include "network/RendezvousClient.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/HttpServer.h"
#include "streaming/SdpFingerprint.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMetaObject>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>

#include <rtc/rtc.hpp>

namespace {

/// Stand-in address for a browser whose real one we have not learned yet.
///
/// It is a label, not an address, and writing it as one keeps it honest: the
/// browser's address is seen by the introduction server and by nobody else.
/// Anything that classifies it finds it is neither loopback nor private, which
/// is the right answer to "may this caller skip the login".
QString peerLabel(const QString& sessionId)
{
    return QStringLiteral("tunnel:") + sessionId.left(16);
}



/// Headers a browser may not dictate over the tunnel.
///
/// The admin key is only ever handed to a local caller, so accepting one off the
/// wire would undo that in a line. The rest are artefacts of a hop that does not
/// exist here, and Content-Length is recomputed from what actually arrived.
/// Everything else is forwarded untouched — this is a proxy, and a proxy that
/// edits what it carries stops being predictable.
bool isForbiddenHeader(const QString& name)
{
    static const QStringList kBlocked = {
        QStringLiteral("x-mw-admin-key"), QStringLiteral("connection"),
        QStringLiteral("upgrade"),        QStringLiteral("transfer-encoding"),
        QStringLiteral("content-length"), QStringLiteral("expect"),
        QStringLiteral("keep-alive"),     QStringLiteral("te"),
    };
    return kBlocked.contains(name);
}

constexpr int kMaxSocketsPerPeer = 8;
constexpr int kMaxInFlightRequests = 24;

} // namespace

ControlTunnel::ControlTunnel(HttpServer* http, AuthManager* auth, AppSettings* settings,
                             RendezvousClient* rendezvous, QObject* parent)
    : QObject(parent), m_Http(http), m_Auth(auth), m_Settings(settings), m_Rendezvous(rendezvous)
{
    connect(m_Rendezvous, &RendezvousClient::sessionOpened, this, &ControlTunnel::onSessionOpened);
    connect(m_Rendezvous, &RendezvousClient::sessionClosed, this, &ControlTunnel::onSessionClosed);
    connect(m_Rendezvous, &RendezvousClient::signalReceived, this, &ControlTunnel::onSignal);

    Logger::info(QStringLiteral("[Tunnel] Ready — host key %1").arg(hostKeyFingerprint()));
}

ControlTunnel::~ControlTunnel()
{
    const QStringList ids = m_Peers.keys();
    for (const QString& id : ids) dropPeer(id, QStringLiteral("shutting down"));
}

QString ControlTunnel::hostKeyFingerprint() const
{
    const QByteArray spki = m_Settings->hostSigningPublicKey();
    if (spki.isEmpty()) return {};
    const QByteArray digest = QCryptographicHash::hash(spki, QCryptographicHash::Sha256);

    QStringList parts;
    parts.reserve(digest.size());
    for (char c : digest)
        parts << QStringLiteral("%1").arg(static_cast<quint8>(c), 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

ControlTunnel::Peer* ControlTunnel::peer(const QString& sessionId)
{
    auto it = m_Peers.find(sessionId);
    return it == m_Peers.end() ? nullptr : it.value().get();
}

QString ControlTunnel::peerAddress(const Peer& p) const
{
    // The address ICE actually settled on. This is not something the browser
    // told us — it is where our own stack is sending packets — which makes it
    // the one trustworthy statement about where this peer sits, and it is what
    // lets a browser on the same LAN keep its direct path when it launches a
    // stream. It never grants privilege: serveRequest() shuts the local
    // exemption for every tunnel arrival regardless of what this returns.
    if (!p.pc) return peerLabel(p.sessionId);
    try {
        rtc::Candidate local;
        rtc::Candidate remote;
        if (p.pc->getSelectedCandidatePair(&local, &remote)) {
            const auto addr = remote.address();
            if (addr.has_value() && !addr->empty()) return QString::fromStdString(*addr);
        }
    } catch (const std::exception&) {
        // No pair yet, or none that can be described. The label stands in.
    }
    return peerLabel(p.sessionId);
}

// ── Session lifecycle ───────────────────────────────────────────────────────

void ControlTunnel::onSessionOpened(const QString& sessionId)
{
    if (m_Peers.contains(sessionId)) {
        Logger::warning(QStringLiteral("[Tunnel] Session %1 announced twice").arg(sessionId));
        return;
    }

    auto owned = std::make_shared<Peer>();
    Peer& p = *owned;
    p.sessionId = sessionId;
    m_Peers.insert(sessionId, std::move(owned));

    // ICE for the control channel.
    //
    // STUN is always on and LAN host candidates are always emitted, which is a
    // deliberate departure from the stream path. There, the browser's address
    // comes from /start and the two cases can be told apart; here it is not
    // known yet, and the two failure modes are not symmetric. Withholding the
    // LAN candidates costs a browser on the same network its direct path — on a
    // router without hairpin NAT, any path at all — while emitting them
    // discloses a private address that says nothing about how to reach it, to
    // someone already holding a 128-bit identifier for this machine.
    rtc::Configuration config;
    config.iceTransportPolicy = rtc::TransportPolicy::All;
    config.enableIceTcp = true;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    p.pc = std::make_shared<rtc::PeerConnection>(config);

    const QString id = sessionId;

    p.pc->onLocalDescription([this, id](const rtc::Description& sdp) {
        const std::string text(sdp);
        QMetaObject::invokeMethod(
            this,
            [this, id, text]() {
                Peer* pp = peer(id);
                if (!pp) return;
                pp->pendingOffer = text;
                pp->localFingerprint = SdpFingerprint::extract(QString::fromStdString(text));
                if (pp->localFingerprint.isEmpty()) {
                    abort(*pp, QStringLiteral("our own offer carries no single sha-256 "
                                              "fingerprint"));
                    return;
                }
                // The host's signature covers the browser's nonce, so the offer
                // waits for the hello that carries it. That ordering is what
                // lets the browser check us BEFORE it creates any DTLS state.
                sendSignedOffer(*pp);
            },
            Qt::QueuedConnection);
    });

    p.pc->onLocalCandidate([this, id](const rtc::Candidate& candidate) {
        const std::string cand = candidate.candidate();
        const std::string mid = candidate.mid();
        QMetaObject::invokeMethod(
            this,
            [this, id, cand, mid]() {
                if (!m_Peers.contains(id)) return;
                m_Rendezvous->sendSignal(
                    id, QJsonObject{{QStringLiteral("type"), QStringLiteral("ice")},
                                    {QStringLiteral("candidate"), QString::fromStdString(cand)},
                                    {QStringLiteral("mid"), QString::fromStdString(mid)}});
            },
            Qt::QueuedConnection);
    });

    p.pc->onStateChange([this, id](rtc::PeerConnection::State state) {
        if (state != rtc::PeerConnection::State::Failed &&
            state != rtc::PeerConnection::State::Closed)
            return;
        QMetaObject::invokeMethod(
            this, [this, id]() { dropPeer(id, QStringLiteral("the peer connection ended")); },
            Qt::QueuedConnection);
    });

    // The host is the offerer, as it is on the stream path — creating the
    // channel is what makes libdatachannel produce the offer.
    p.dc = p.pc->createDataChannel("mw-ctl");

    p.dc->onMessage(
        [this, id](rtc::binary data) {
            const QByteArray bytes(reinterpret_cast<const char*>(data.data()),
                                   static_cast<qsizetype>(data.size()));
            QMetaObject::invokeMethod(
                this, [this, id, bytes]() { onChannelMessage(id, bytes); }, Qt::QueuedConnection);
        },
        [](rtc::string) { /* the tunnel speaks binary only */ });

    p.dc->onBufferedAmountLow([this, id]() {
        QMetaObject::invokeMethod(
            this,
            [this, id]() {
                if (Peer* pp = peer(id)) drain(*pp);
            },
            Qt::QueuedConnection);
    });
    p.dc->setBufferedAmountLowThreshold(TunnelFrame::kChunkBytes);

    Logger::info(QStringLiteral("[Tunnel] Browser arrived on session %1").arg(sessionId.left(8)));
}

void ControlTunnel::onSessionClosed(const QString& sessionId)
{
    dropPeer(sessionId, QStringLiteral("the browser left"));
}

void ControlTunnel::dropPeer(const QString& sessionId, const QString& why)
{
    auto it = m_Peers.find(sessionId);
    if (it == m_Peers.end()) return;

    const std::shared_ptr<Peer> owned = it.value();
    m_Peers.erase(it);

    for (auto& ws : owned->sockets) {
        if (ws) {
            ws->abort();
            ws->deleteLater();
        }
    }
    owned->sockets.clear();

    // Detach the callbacks before the objects go. libdatachannel may still be
    // inside one on a thread of its own, and it must not come back to a peer
    // that is halfway through being destroyed.
    if (owned->dc) {
        owned->dc->onMessage(nullptr, nullptr);
        owned->dc->onBufferedAmountLow(nullptr);
        owned->dc->close();
    }
    if (owned->pc) {
        owned->pc->onLocalDescription(nullptr);
        owned->pc->onLocalCandidate(nullptr);
        owned->pc->onStateChange(nullptr);
        owned->pc->close();
    }

    Logger::info(QStringLiteral("[Tunnel] Session %1 closed — %2").arg(sessionId.left(8), why));
}

void ControlTunnel::abort(Peer& p, const QString& why)
{
    // The session id is copied out first: dropPeer() destroys the peer this
    // reference points into.
    const QString id = p.sessionId;
    Logger::error(QStringLiteral("[Tunnel] MW-BIND-v1 refused session %1: %2").arg(id.left(8), why));
    m_Rendezvous->closeSession(id);
    dropPeer(id, QStringLiteral("pairing verification failed"));
}

// ── Signalling ──────────────────────────────────────────────────────────────

void ControlTunnel::onSignal(const QString& sessionId, const QJsonObject& payload)
{
    Peer* p = peer(sessionId);
    if (!p) return;

    const QString type = payload.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("hello")) handleHello(*p, payload);
    else if (type == QLatin1String("sdp")) handleAnswer(*p, payload);
    else if (type == QLatin1String("ice")) handleIce(*p, payload);
}

void ControlTunnel::handleHello(Peer& p, const QJsonObject& msg)
{
    if (p.helloSeen) {
        abort(p, QStringLiteral("a second hello on the same connection"));
        return;
    }

    // The browser presents the key it will sign with. On the stream path the
    // host already knows that key, because the session it belongs to was paired
    // with a PIN; here there is no session yet — the login screen is what this
    // channel is being opened to fetch. So the key is taken as presented, and
    // used to bind the channel rather than to say who is holding it.
    p.browserSpki =
        QByteArray::fromBase64(msg.value(QStringLiteral("public_key")).toString().toUtf8());
    if (!PairingCrypto::isValidP256Spki(p.browserSpki)) {
        abort(p, QStringLiteral("the hello does not carry a usable P-256 public key"));
        return;
    }

    p.browserKeyId = PairingCrypto::keyId(p.browserSpki);
    if (msg.value(QStringLiteral("key_id")).toString() != p.browserKeyId) {
        abort(p, QStringLiteral("the hello's key id does not match the key it presented"));
        return;
    }

    p.nonceBrowser = QByteArray::fromBase64(msg.value(QStringLiteral("nonce")).toString().toUtf8());
    if (p.nonceBrowser.size() < PairingCrypto::MIN_NONCE_BYTES) {
        abort(p, QStringLiteral("the hello nonce is too short to be single-use"));
        return;
    }

    p.helloSeen = true;
    sendSignedOffer(p);
}

void ControlTunnel::sendSignedOffer(Peer& p)
{
    if (p.pendingOffer.empty() || !p.helloSeen) return;

    p.nonceHost = PairingCrypto::generateNonce();
    if (p.nonceHost.isEmpty()) {
        abort(p, QStringLiteral("no secure random available for the host nonce"));
        return;
    }

    const QString hostId = m_Settings->rendezvousId();
    const QByteArray toSign =
        PairingCrypto::hostDigestInput(hostId, p.browserKeyId, p.nonceBrowser, p.localFingerprint);
    const QByteArray sig = PairingCrypto::sign(m_Settings->hostSigningKeyPem(), toSign);
    if (sig.isEmpty()) {
        // Sending it unsigned would teach the browser that unsigned offers are
        // acceptable, which is the downgrade this protocol exists to prevent.
        abort(p, QStringLiteral("could not sign the offer with the host key"));
        return;
    }

    m_Rendezvous->sendSignal(
        p.sessionId,
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("sdp")},
            {QStringLiteral("sdp"), QString::fromStdString(p.pendingOffer)},
            {QStringLiteral("protocol"), QString::fromLatin1(PairingCrypto::PROTOCOL)},
            {QStringLiteral("host_id"), hostId},
            {QStringLiteral("host_key"),
             QString::fromLatin1(m_Settings->hostSigningPublicKey().toBase64())},
            {QStringLiteral("nonce"), QString::fromLatin1(p.nonceHost.toBase64())},
            {QStringLiteral("sig"), QString::fromLatin1(sig.toBase64())},
        });
    p.pendingOffer.clear();
}

void ControlTunnel::handleAnswer(Peer& p, const QJsonObject& msg)
{
    if (p.answerVerified) return;
    if (!p.helloSeen) {
        abort(p, QStringLiteral("an answer arrived before the hello"));
        return;
    }

    const QString sdp = msg.value(QStringLiteral("sdp")).toString();
    const QString remoteFingerprint = SdpFingerprint::extract(sdp);
    if (remoteFingerprint.isEmpty()) {
        abort(p, QStringLiteral("the answer does not commit to one sha-256 fingerprint"));
        return;
    }

    const QByteArray sig =
        QByteArray::fromBase64(msg.value(QStringLiteral("sig")).toString().toUtf8());
    if (sig.isEmpty()) {
        abort(p, QStringLiteral("the answer carries no signature"));
        return;
    }

    const QByteArray signedBytes = PairingCrypto::browserDigestInput(
        m_Settings->rendezvousId(), p.nonceHost, p.localFingerprint, remoteFingerprint);
    if (!PairingCrypto::verify(p.browserSpki, signedBytes, sig)) {
        abort(p, QStringLiteral("the answer's signature does not verify"));
        return;
    }

    p.answerVerified = true;
    try {
        p.pc->setRemoteDescription(rtc::Description(sdp.toStdString()));
    } catch (const std::exception& e) {
        abort(p, QStringLiteral("the answer was not usable: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    Logger::info(QStringLiteral("[Tunnel] Session %1 bound — both fingerprints verified, key %2")
                     .arg(p.sessionId.left(8), p.browserKeyId.left(12)));
}

void ControlTunnel::handleIce(Peer& p, const QJsonObject& msg)
{
    // Candidates are taken only once the answer has verified: before that there
    // is no remote description to attach them to, and doing the work anyway
    // would be work done on behalf of a peer that has not proved anything.
    if (!p.answerVerified) return;
    try {
        p.pc->addRemoteCandidate(
            rtc::Candidate(msg.value(QStringLiteral("candidate")).toString().toStdString(),
                           msg.value(QStringLiteral("mid")).toString().toStdString()));
    } catch (const std::exception&) {
        // A candidate we cannot parse is one path out of several, not a failure.
    }
}

// ── The channel ─────────────────────────────────────────────────────────────

void ControlTunnel::sendFrame(Peer& p, const QByteArray& frame)
{
    p.outbox.append(frame);
    drain(p);
}

void ControlTunnel::drain(Peer& p)
{
    if (p.draining || !p.dc || !p.dc->isOpen()) return;
    p.draining = true;

    // Stop pushing once the channel has a comfortable backlog, and let
    // onBufferedAmountLow bring us back. Handing a whole response over at once
    // would park it in libdatachannel's queue, where a browser that walked away
    // leaves it stranded.
    while (!p.outbox.isEmpty() && p.dc->bufferedAmount() < 4 * TunnelFrame::kChunkBytes) {
        const QByteArray frame = p.outbox.takeFirst();
        try {
            p.dc->send(reinterpret_cast<const std::byte*>(frame.constData()),
                       static_cast<size_t>(frame.size()));
        } catch (const std::exception& e) {
            Logger::warning(QStringLiteral("[Tunnel] Send failed on session %1: %2")
                                .arg(p.sessionId.left(8), QString::fromUtf8(e.what())));
            p.outbox.clear();
            break;
        }
    }
    p.draining = false;
}

void ControlTunnel::onChannelMessage(const QString& sessionId, const QByteArray& message)
{
    Peer* pp = peer(sessionId);
    if (!pp) return;

    quint8 kind = 0;
    quint32 id = 0;
    QByteArray payload;
    if (!TunnelFrame::parse(message, &kind, &id, &payload)) return;

    switch (kind) {
    case TunnelFrame::Request: handleRequestFrame(*pp, id, payload); break;
    case TunnelFrame::WsOpen: handleWsOpen(*pp, id, payload); break;
    case TunnelFrame::WsText: handleWsText(*pp, id, QString::fromUtf8(payload)); break;
    case TunnelFrame::WsClose: closeWs(*pp, id, QString::fromUtf8(payload)); break;
    default: break;
    }
}

void ControlTunnel::handleRequestFrame(Peer& p, quint32 id, const QByteArray& payload)
{
    if (payload.size() > TunnelFrame::kMaxRequestBytes) {
        sendResponse(p.sessionId, id, HttpResponse::error(413, "Payload Too Large"), QString());
        return;
    }
    if (p.inFlight >= kMaxInFlightRequests) {
        sendResponse(p.sessionId, id, HttpResponse::error(429, "Too Many Requests"), QString());
        return;
    }

    QJsonObject head;
    QByteArray body;
    if (!TunnelFrame::decodeHead(payload, &head, &body)) return;

    HttpRequest req;
    req.method = head.value(QStringLiteral("m")).toString().toUpper();
    req.body = body;
    req.clientAddress = peerAddress(p);

    // Split the target the way HttpParser does for a socket request, so a route
    // sees the same shape whichever way the request arrived.
    const QUrl target(head.value(QStringLiteral("p")).toString(), QUrl::StrictMode);
    if (!target.isValid() || !target.path().startsWith(QLatin1Char('/'))) {
        sendResponse(p.sessionId, id, HttpResponse::error(400, "Bad Request"), QString());
        return;
    }
    req.path = target.path();
    const QUrlQuery query(target);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto& item : items) req.queryParams.insert(item.first, item.second);

    const QJsonObject headers = head.value(QStringLiteral("h")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const QString name = it.key().toLower();
        if (isForbiddenHeader(name)) continue;
        req.headers.insert(name, it.value().toString());
    }
    req.headers.insert(QStringLiteral("content-length"), QString::number(req.body.size()));

    const QString hostHeader = req.headers.value(QStringLiteral("host"));
    const QString sessionId = p.sessionId;
    p.inFlight++;

    m_Http->serveRequest(std::move(req), HttpServer::Arrival::Tunnel,
                         [this, sessionId, id, hostHeader](const HttpResponse& resp) {
                             Peer* pp = peer(sessionId);
                             if (!pp) return;
                             pp->inFlight--;
                             sendResponse(sessionId, id, resp, hostHeader);
                         });
}

void ControlTunnel::sendResponse(const QString& sessionId, quint32 id, const HttpResponse& response,
                                 const QString& hostHeader)
{
    Peer* p = peer(sessionId);
    if (!p) return;

    // The same security headers a socket response carries. They are not
    // decoration here: the service worker hands this back as the document's own
    // Response, so this Content-Security-Policy is the one the browser enforces
    // on the page.
    QJsonObject headers;
    const QMap<QString, QString> security = HttpServer::securityHeaders(hostHeader);
    for (auto it = security.cbegin(); it != security.cend(); ++it)
        headers.insert(it.key(), it.value());
    if (!response.contentType.isEmpty())
        headers.insert(QStringLiteral("Content-Type"), response.contentType);
    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
        headers.insert(it.key(), it.value());

    const QByteArray head = TunnelFrame::encodeHead(
        QJsonObject{{QStringLiteral("s"), response.statusCode}, {QStringLiteral("h"), headers}},
        QByteArray());
    sendFrame(*p, TunnelFrame::build(TunnelFrame::Response, id, head));

    for (qsizetype offset = 0; offset < response.body.size(); offset += TunnelFrame::kChunkBytes)
        sendFrame(*p, TunnelFrame::build(TunnelFrame::Body, id,
                                         response.body.mid(offset, TunnelFrame::kChunkBytes)));

    sendFrame(*p, TunnelFrame::build(TunnelFrame::End, id, QByteArray()));
}

// ── Streaming signalling, carried through ───────────────────────────────────

void ControlTunnel::handleWsOpen(Peer& p, quint32 id, const QByteArray& payload)
{
    QJsonObject head;
    if (!TunnelFrame::decodeHead(payload, &head, nullptr)) return;

    if (p.sockets.size() >= kMaxSocketsPerPeer) {
        closeWs(p, id, QStringLiteral("too many signalling sockets"));
        return;
    }

    // The same authorisation a WebSocket upgrade gets on the socket path,
    // decided by HttpServer so there is one gate and not two. The cookies come
    // from the browser's own jar, kept by the service worker; nothing here can
    // be more privileged than the session behind them.
    HttpRequest asRequest;
    asRequest.method = QStringLiteral("GET");
    asRequest.path = head.value(QStringLiteral("p")).toString();
    asRequest.clientAddress = peerAddress(p);
    const QJsonObject headers = head.value(QStringLiteral("h")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it)
        asRequest.headers.insert(it.key().toLower(), it.value().toString());

    QString error;
    const quint16 port = m_Http->authorizeTunnelWebSocket(asRequest, &error);
    if (port == 0) {
        closeWs(p, id, error);
        return;
    }

    QWebSocket* ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    p.sockets.insert(id, ws);

    const QString sessionId = p.sessionId;

    connect(ws, &QWebSocket::connected, this, [this, sessionId, id]() {
        if (Peer* pp = peer(sessionId)) sendFrame(*pp, TunnelFrame::build(TunnelFrame::WsOpened, id, QByteArray()));
    });
    connect(ws, &QWebSocket::textMessageReceived, this,
            [this, sessionId, id](const QString& text) {
                if (Peer* pp = peer(sessionId))
                    sendFrame(*pp, TunnelFrame::build(TunnelFrame::WsText, id, text.toUtf8()));
            });
    connect(ws, &QWebSocket::disconnected, this, [this, sessionId, id]() {
        Peer* pp = peer(sessionId);
        if (!pp) return;
        if (QPointer<QWebSocket> gone = pp->sockets.take(id)) gone->deleteLater();
        sendFrame(*pp, TunnelFrame::build(TunnelFrame::WsClose, id, QByteArray()));
    });

    ws->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/").arg(port)));
}

void ControlTunnel::handleWsText(Peer& p, quint32 id, const QString& text)
{
    QPointer<QWebSocket> ws = p.sockets.value(id);
    if (ws && ws->isValid()) ws->sendTextMessage(text);
}

void ControlTunnel::closeWs(Peer& p, quint32 id, const QString& reason)
{
    if (QPointer<QWebSocket> ws = p.sockets.take(id)) {
        ws->close();
        ws->deleteLater();
    }
    sendFrame(p, TunnelFrame::build(TunnelFrame::WsClose, id, reason.toUtf8()));
}
