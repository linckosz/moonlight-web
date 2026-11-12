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

#include "common/Types.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

/**
 * The channel between the running server and the other programs of this
 * install on the same machine: the tray icon of a server that runs as a
 * service, the command line, a second launch.
 *
 * It is not a network socket. On Windows it is a named pipe that refuses
 * remote clients, open to the server's own account and to the people signed in
 * at this PC; elsewhere a Unix socket under the server's data folder, open to
 * the server's account. Whoever reaches it is on this machine by construction,
 * which no address on a TCP connection can say.
 *
 * Nor does it speak HTTP: a line of its own, then one length-prefixed request
 * and one length-prefixed answer per connection, so nothing that forwards HTTP
 * can be pointed at it. An answer carries a status and a body, and no header:
 * no cookie can be set through here.
 *
 * The wire shapes are pure and tested on every platform; the two ends are in
 * LocalChannel.cpp (tests/test_local_channel.cpp).
 */
namespace mw::localchannel {

/// What a caller asks: a route of the server's own API.
struct Request
{
    QString method;  ///< "GET", "POST", …
    QString path;    ///< "/api/…", no query
    QString query;   ///< "a=b&c=d", may be empty
    QByteArray body; ///< JSON as the route reads it, may be empty
};

/// What it is answered. @c status 0: the request never got an answer.
struct Reply
{
    int status = 0;
    QByteArray body;
};

// ── The wire ────────────────────────────────────────────────────────────────

/// The line a caller opens with. Anything else is not this protocol.
inline constexpr char kPreamble[] = "MW-LOCAL-1\n";
/// The largest request, and the largest answer, a frame may carry.
inline constexpr quint32 kMaxRequestBytes = 64 * 1024;
inline constexpr quint32 kMaxReplyBytes = 1024 * 1024;

/// @p payload behind its length: four bytes, most significant first.
QByteArray frame(const QByteArray& payload);

QByteArray encodeRequest(const Request& request);
/// Empty for anything but a well-formed request: a method in capitals, a path
/// from the root with no query and no control character.
std::optional<Request> decodeRequest(const QByteArray& payload);

QByteArray encodeReply(const Reply& reply);
std::optional<Reply> decodeReply(const QByteArray& payload);

/// One end's bytes as they arrive: the opening line when @p expectPreamble,
/// then one frame of at most @p maxBytes. A byte after the frame is refused:
/// one request, one answer.
class FrameReader
{
public:
    enum class State
    {
        NeedMore,
        Ready,
        Bad,
    };

    FrameReader(bool expectPreamble, quint32 maxBytes);

    State feed(const QByteArray& bytes);
    State state() const { return m_State; }
    /// The frame's payload, once Ready.
    QByteArray payload() const { return m_Payload; }
    /// Why it is Bad.
    QString error() const { return m_Error; }

private:
    State parse();

    bool m_ExpectPreamble;
    quint32 m_MaxBytes;
    QByteArray m_Buffer;
    QByteArray m_Payload;
    QString m_Error;
    State m_State = State::NeedMore;
};

// ── Where it is ─────────────────────────────────────────────────────────────

/// The channel of one install: "<product>-local", "<product>-dev-local" for a
/// --dev run, so the two never answer for each other.
QString channelName(const QString& productName, bool devFlag);

/// The pipe's path on Windows; elsewhere the socket's full path, in
/// @p dataDir (the server's data folder, never a temporary one).
QString endpointFor(const QString& channelName, const QString& dataDir);

/// Who may open the Windows pipe, as the text its access list is built from,
/// for a server whose account is @p ownerSid. Protected, so nothing is
/// inherited. That account, in full. The people signed in at this PC, to read
/// and to write data, which is asking and being answered: not to make an
/// instance of the pipe, so the name stays the server's. Nobody else has an
/// entry — not everyone, not every signed-in account of the network, not the
/// anonymous — and the pipe refuses remote clients on top of it.
QString pipeAccessList(const QString& ownerSid);

// ── The server's end ────────────────────────────────────────────────────────

class Server : public QObject
{
public:
    using Respond = std::function<void(const Reply&)>;
    /// Called on the thread the Server lives on. @c respond may be called
    /// later, once, from that same thread.
    using Handler = std::function<void(const Request&, Respond)>;

    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    /// Opens the channel. False, with @p error, when the name is already held
    /// by something else or cannot be made: the server then has no channel, and
    /// says so, rather than one it shares.
    bool listen(const QString& endpoint, Handler handler, QString* error = nullptr);
    void close();
    bool isListening() const;
    QString endpoint() const;

    /// The platform's side of it (LocalChannel.cpp): nothing a caller uses.
    struct Impl;

private:
    std::shared_ptr<Impl> m_Impl;
};

// ── Into the server ─────────────────────────────────────────────────────────

/// What a request of this channel is given for an address: a label. Nothing
/// is granted on it, and no failure is counted against it.
inline constexpr char kClientLabel[] = "local-channel";

/// The request the server's router is handed for one asked over the channel.
/// It has no header of the caller's, for the frame carries none: no cookie, no
/// Host, no forwarding header. The content type of a body is stated here, by
/// the server's own code, because every route reads JSON and the access
/// decision checks that it was told so.
HttpRequest httpRequestFor(const Request& request);

/// What goes back of the server's answer: its status and its body, no header.
Reply replyFor(const HttpResponse& response);

// ── A caller's end ──────────────────────────────────────────────────────────

/// One request, waited for at most @p timeoutMs. A reply with status 0 and
/// @p error set when nothing listens, when what listens is not this install's
/// server, or when no answer came in time.
Reply request(const QString& endpoint, const Request& request, int timeoutMs,
              QString* error = nullptr);

} // namespace mw::localchannel
