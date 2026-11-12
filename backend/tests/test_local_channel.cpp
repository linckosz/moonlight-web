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

// The channel between the running server and the other programs of its install
// (server/LocalChannel.h): its wire shapes, then both of its ends for real.

#include "server/LocalChannel.h"
#include "test_framework.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>
#include <vector>

namespace {

using namespace mw::localchannel;

/// Pump this thread's events until @p done or @p ms pass.
template <class F> bool waitFor(F done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

/// One request from another thread, as the command line makes it from another
/// process, while this thread plays the server.
struct Asked
{
    Reply reply;
    QString error;
};

Asked ask(const QString& endpoint, const Request& request, int timeoutMs = 4000)
{
    auto out = std::make_shared<Asked>();
    QThread* caller = QThread::create([out, endpoint, request, timeoutMs]() {
        out->reply = mw::localchannel::request(endpoint, request, timeoutMs, &out->error);
    });
    caller->start();
    waitFor([caller]() { return caller->isFinished(); }, timeoutMs + 3000);
    caller->wait(2000);
    delete caller;
    return *out;
}

/// A caller that writes @p bytes as they are and reports what came back: for
/// the things a well-behaved caller never sends.
struct RawResult
{
    bool connected = false;
    bool closedByServer = false;
    QByteArray received;
};

RawResult sendRaw(const QString& endpoint, const QByteArray& bytes, int waitMs = 2500)
{
    auto out = std::make_shared<RawResult>();
    QThread* caller = QThread::create([out, endpoint, bytes, waitMs]() {
        QLocalSocket socket;
        socket.connectToServer(endpoint);
        if (!socket.waitForConnected(2000)) return;
        out->connected = true;
        socket.write(bytes);
        socket.waitForBytesWritten(1000);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < waitMs && socket.state() == QLocalSocket::ConnectedState) {
            socket.waitForReadyRead(50);
            out->received += socket.readAll();
        }
        out->received += socket.readAll();
        out->closedByServer = socket.state() != QLocalSocket::ConnectedState;
    });
    caller->start();
    waitFor([caller]() { return caller->isFinished(); }, waitMs + 5000);
    caller->wait(2000);
    delete caller;
    return *out;
}

QByteArray lengthOnly(quint32 n)
{
    QByteArray out;
    out.append(static_cast<char>((n >> 24) & 0xff));
    out.append(static_cast<char>((n >> 16) & 0xff));
    out.append(static_cast<char>((n >> 8) & 0xff));
    out.append(static_cast<char>(n & 0xff));
    return out;
}

} // namespace

void run_local_channel_tests()
{
    SECTION("Local channel — a frame is its payload behind four bytes, most significant first");
    {
        const QByteArray f = frame(QByteArray("abc"));
        CHECK_EQ(f.size(), 7);
        CHECK(f.left(4) == QByteArray::fromHex("00000003"));
        CHECK(f.mid(4) == QByteArray("abc"));
        CHECK(frame(QByteArray(300, 'x')).left(4) == QByteArray::fromHex("0000012c"));
    }

    SECTION("Local channel — the reader takes the opening line, then one frame");
    {
        const QByteArray wire = QByteArray(kPreamble) + frame(QByteArray("{\"a\":1}"));

        FrameReader whole(true, kMaxRequestBytes);
        CHECK(whole.feed(wire) == FrameReader::State::Ready);
        CHECK(whole.payload() == QByteArray("{\"a\":1}"));

        // Byte by byte, as a slow caller would send it.
        FrameReader slow(true, kMaxRequestBytes);
        for (int i = 0; i < wire.size() - 1; ++i)
            CHECK(slow.feed(wire.mid(i, 1)) == FrameReader::State::NeedMore);
        CHECK(slow.feed(wire.right(1)) == FrameReader::State::Ready);
        CHECK(slow.payload() == QByteArray("{\"a\":1}"));

        // An answer has no opening line.
        FrameReader answer(false, kMaxReplyBytes);
        CHECK(answer.feed(frame(QByteArray("ok"))) == FrameReader::State::Ready);
        CHECK(answer.payload() == QByteArray("ok"));
    }

    SECTION("Local channel — anything but its own opening line is refused at the first byte");
    {
        FrameReader http(true, kMaxRequestBytes);
        CHECK(http.feed(QByteArray("G")) == FrameReader::State::Bad);
        CHECK(!http.error().isEmpty());

        FrameReader whole(true, kMaxRequestBytes);
        CHECK(whole.feed(QByteArray("GET /api/admin/token HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) ==
              FrameReader::State::Bad);

        // The right start and then something else.
        FrameReader half(true, kMaxRequestBytes);
        CHECK(half.feed(QByteArray("MW-LOCAL-")) == FrameReader::State::NeedMore);
        CHECK(half.feed(QByteArray("2\n")) == FrameReader::State::Bad);
        // Refused stays refused.
        CHECK(half.feed(QByteArray(kPreamble)) == FrameReader::State::Bad);
    }

    SECTION("Local channel — a frame's length is judged before its bytes are taken");
    {
        FrameReader huge(true, kMaxRequestBytes);
        CHECK(huge.feed(QByteArray(kPreamble) + lengthOnly(kMaxRequestBytes + 1)) ==
              FrameReader::State::Bad);

        FrameReader most(true, kMaxRequestBytes);
        CHECK(most.feed(QByteArray(kPreamble) + lengthOnly(0x7fffffff)) == FrameReader::State::Bad);

        FrameReader empty(true, kMaxRequestBytes);
        CHECK(empty.feed(QByteArray(kPreamble) + lengthOnly(0)) == FrameReader::State::Bad);

        // The largest allowed is taken.
        FrameReader edge(true, kMaxRequestBytes);
        CHECK(edge.feed(QByteArray(kPreamble) + lengthOnly(kMaxRequestBytes)) ==
              FrameReader::State::NeedMore);
        CHECK(edge.feed(QByteArray(static_cast<int>(kMaxRequestBytes), 'x')) ==
              FrameReader::State::Ready);
    }

    SECTION("Local channel — one request per connection: a byte after the frame is refused");
    {
        FrameReader together(true, kMaxRequestBytes);
        CHECK(together.feed(QByteArray(kPreamble) + frame(QByteArray("{}")) + QByteArray("x")) ==
              FrameReader::State::Bad);

        FrameReader later(true, kMaxRequestBytes);
        CHECK(later.feed(QByteArray(kPreamble) + frame(QByteArray("{}"))) ==
              FrameReader::State::Ready);
        CHECK(later.feed(QByteArray()) == FrameReader::State::Ready);
        CHECK(later.feed(frame(QByteArray("{}"))) == FrameReader::State::Bad);
    }

    SECTION("Local channel — a request goes over and comes back whole");
    {
        Request r;
        r.method = QStringLiteral("POST");
        r.path = QStringLiteral("/api/admin/pin/generate");
        r.query = QStringLiteral("p=/admin");
        r.body = QByteArray("{\"a\":\"\xc3\xa9\\u0000\"}");
        const std::optional<Request> back = decodeRequest(encodeRequest(r));
        CHECK(back.has_value());
        if (back) {
            CHECK(back->method == r.method);
            CHECK(back->path == r.path);
            CHECK(back->query == r.query);
            CHECK(back->body == r.body);
        }

        Request bare;
        bare.method = QStringLiteral("GET");
        bare.path = QStringLiteral("/api/server/status");
        const std::optional<Request> bareBack = decodeRequest(encodeRequest(bare));
        CHECK(bareBack.has_value());
        if (bareBack) {
            CHECK(bareBack->query.isEmpty());
            CHECK(bareBack->body.isEmpty());
        }
    }

    SECTION("Local channel — a request names a path of this server and nothing else");
    {
        auto refused = [](const char* json) {
            return !decodeRequest(QByteArray(json)).has_value();
        };
        CHECK(!refused(R"({"method":"GET","path":"/api/health"})"));

        // Not a path from the root; a scheme, a host; a query or a fragment in it.
        CHECK(refused(R"({"method":"GET","path":"api/health"})"));
        CHECK(refused(R"({"method":"GET","path":""})"));
        CHECK(refused(R"({"method":"GET","path":"//other.example/api/health"})"));
        CHECK(refused(R"({"method":"GET","path":"http://other.example/api/health"})"));
        CHECK(refused(R"({"method":"GET","path":"/x/http://other.example/"})"));
        CHECK(refused(R"({"method":"GET","path":"/api/health?x=1"})"));
        CHECK(refused(R"({"method":"GET","path":"/api/health#k=1"})"));
        CHECK(refused(R"({"method":"GET","path":"/api\\health"})"));
        // A control character, as a smuggled header line would need.
        CHECK(refused(R"({"method":"GET","path":"/api/health\r\nHost: x"})"));
        CHECK(refused(R"({"method":"GET","path":"/api/health","query":"a=1\r\nCookie: x"})"));
        CHECK(refused(R"({"method":"GET","path":"/api/health","query":"a=1#b"})"));

        // The verb in capitals, and a verb at all.
        CHECK(refused(R"({"method":"get","path":"/api/health"})"));
        CHECK(refused(R"({"method":"GET /x","path":"/api/health"})"));
        CHECK(refused(R"({"path":"/api/health"})"));
        CHECK(refused(R"({"method":7,"path":"/api/health"})"));
        CHECK(refused(R"({"method":"GET","path":7})"));
        CHECK(refused(R"({"method":"GET","path":"/api/health","query":7})"));

        // A field this channel does not know: headers least of all.
        CHECK(refused(R"({"method":"GET","path":"/api/health","headers":{"cookie":"x"}})"));
        CHECK(refused(R"({"method":"GET","path":"/api/health","host":"localhost"})"));

        // The body is base64 text, whole.
        CHECK(refused(R"({"method":"POST","path":"/api/x","body":"not base64!"})"));
        CHECK(refused(R"({"method":"POST","path":"/api/x","body":{"a":1}})"));

        // Not an object, not JSON.
        CHECK(refused(R"(["GET","/api/health"])"));
        CHECK(refused("GET /api/health HTTP/1.1"));
        CHECK(refused(""));
    }

    SECTION("Local channel — an answer is a status and a body, and nothing else");
    {
        Reply r;
        r.status = 200;
        r.body = QByteArray("{\"ok\":true}");
        const std::optional<Reply> back = decodeReply(encodeReply(r));
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(back->status, 200);
            CHECK(back->body == r.body);
        }
        // No header can come back: the shape has no place for one.
        CHECK(!encodeReply(r).contains("Set-Cookie"));
        CHECK(!decodeReply(QByteArray(R"({"status":200,"headers":{"Set-Cookie":"a=b"}})"))
                   .has_value());
        CHECK(!decodeReply(QByteArray(R"({"status":0})")).has_value());
        CHECK(!decodeReply(QByteArray(R"({"status":700})")).has_value());
        CHECK(!decodeReply(QByteArray(R"({"body":"eA=="})")).has_value());
        CHECK(!decodeReply(QByteArray("HTTP/1.1 200 OK")).has_value());
    }

    SECTION("Local channel — one name per install, apart for a --dev run");
    {
        CHECK(channelName(QStringLiteral("MoonlightWeb"), false) ==
              QStringLiteral("MoonlightWeb-local"));
        CHECK(channelName(QStringLiteral("MoonlightWeb"), true) ==
              QStringLiteral("MoonlightWeb-dev-local"));
        CHECK(channelName(QStringLiteral("MoonlightWebDev"), false) !=
              channelName(QStringLiteral("MoonlightWeb"), true));
        const QString where =
            endpointFor(QStringLiteral("MoonlightWeb-local"), QStringLiteral("/data/mw"));
#ifdef Q_OS_WIN
        CHECK(where == QStringLiteral("\\\\.\\pipe\\MoonlightWeb-local"));
#else
        // Under the data folder, never a temporary one.
        CHECK(where == QStringLiteral("/data/mw/channel/local.sock"));
#endif
    }

    SECTION("Local channel — the Windows pipe opens to the server's account and to who is signed "
            "in at the PC, and to nobody else");
    {
        const QString sid = QStringLiteral("S-1-5-21-1111-2222-3333-1001");
        const QString list = pipeAccessList(sid);
        // The server's account owns it, and the list is protected: nothing is
        // inherited from above.
        CHECK(list.startsWith(QStringLiteral("O:") + sid + QStringLiteral("D:P(")));

        // Two entries, both of them allowing, and they end the list.
        const QRegularExpression entry(QStringLiteral("\\(([A-Z]+);;([0-9A-Za-zx]+);;;([^)]+)\\)"));
        QStringList kinds;
        QStringList rights;
        QStringList who;
        auto it = entry.globalMatch(list);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            kinds.append(m.captured(1));
            rights.append(m.captured(2));
            who.append(m.captured(3));
        }
        CHECK_EQ(static_cast<int>(who.size()), 2);
        CHECK_EQ(list.count(QLatin1Char('(')), 2);
        CHECK(list.endsWith(QLatin1Char(')')));
        if (who.size() == 2) {
            CHECK(kinds.at(0) == QStringLiteral("A"));
            CHECK(kinds.at(1) == QStringLiteral("A"));
            // The server's account, in full.
            CHECK(who.at(0) == sid);
            CHECK(rights.at(0) == QStringLiteral("GA"));
            // Who is signed in at this PC: to read and to write data. Not to
            // make an instance of the pipe (0x4), which would let them answer
            // in the server's place; not to change its list or its owner.
            CHECK(who.at(1) == QStringLiteral("IU"));
            bool ok = false;
            const uint mask = rights.at(1).toUInt(&ok, 16);
            CHECK(ok);
            CHECK((mask & 0x1u) != 0);     // FILE_READ_DATA
            CHECK((mask & 0x2u) != 0);     // FILE_WRITE_DATA
            CHECK((mask & 0x4u) == 0);     // FILE_CREATE_PIPE_INSTANCE
            CHECK((mask & 0x10000u) == 0); // DELETE
            CHECK((mask & 0x40000u) == 0); // WRITE_DAC
            CHECK((mask & 0x80000u) == 0); // WRITE_OWNER
        }
        // Nobody else: not everyone, not the signed-in accounts of the
        // network, not the anonymous, not who comes over the network.
        for (const char* other : {"WD", "AU", "AN", "NU", "BU", "BG", "LG"})
            CHECK(!who.contains(QLatin1String(other)));
        // And no entry that refuses: a refusal by group would also refuse the
        // server's own account when it carries that group.
        CHECK(!list.contains(QStringLiteral("(D;")));
    }

    SECTION("Local channel — what the server's router is handed holds no header of the caller's");
    {
        Request r;
        r.method = QStringLiteral("POST");
        r.path = QStringLiteral("/api/admin/pin/generate");
        r.query = QStringLiteral("p=/admin&x=a%20b");
        r.body = QByteArray("{}");
        const HttpRequest http = httpRequestFor(r);
        CHECK(http.method == QStringLiteral("POST"));
        CHECK(http.path == QStringLiteral("/api/admin/pin/generate"));
        CHECK(http.queryParams.value(QStringLiteral("p")) == QStringLiteral("/admin"));
        CHECK(http.queryParams.value(QStringLiteral("x")) == QStringLiteral("a b"));
        CHECK(http.body == QByteArray("{}"));
        // The two headers there are come from the server's own code.
        CHECK_EQ(static_cast<int>(http.headers.size()), 2);
        CHECK(http.headers.value(QStringLiteral("content-type")) ==
              QStringLiteral("application/json"));
        CHECK(http.headers.value(QStringLiteral("content-length")) == QStringLiteral("2"));
        CHECK(!http.headers.contains(QStringLiteral("cookie")));
        CHECK(!http.headers.contains(QStringLiteral("host")));
        CHECK(!http.headers.contains(QStringLiteral("x-mw-admin-key")));
        // A label for an address, and none of the marks another arrival sets.
        CHECK(http.clientAddress == QLatin1String(kClientLabel));
        CHECK(!http.viaTunnel);
        CHECK(http.tunnelKey.isEmpty());
        CHECK(!http.isLocal);
        CHECK(!http.isHostMachine);

        // No body: no content type to state.
        Request get;
        get.method = QStringLiteral("GET");
        get.path = QStringLiteral("/api/health");
        const HttpRequest bare = httpRequestFor(get);
        CHECK(!bare.headers.contains(QStringLiteral("content-type")));
        CHECK(bare.queryParams.isEmpty());

        // And of the answer, the status and the body go back: no header.
        HttpResponse resp = HttpResponse::json(QJsonObject{{QStringLiteral("ok"), true}}, 201);
        resp.headers[QStringLiteral("Set-Cookie")] = QStringLiteral("mw_session=abc");
        const Reply reply = replyFor(resp);
        CHECK_EQ(reply.status, 201);
        CHECK(reply.body == resp.body);
        CHECK(!encodeReply(reply).contains("mw_session"));
    }

    // ── Both ends, for real ─────────────────────────────────────────────────
    QTemporaryDir data;
    const QString endpoint = endpointFor(
        QStringLiteral("mw-tests-%1-local").arg(QCoreApplication::applicationPid()), data.path());

    SECTION("Local channel — nothing listens: the caller is told so, and has no answer");
    {
        Request r;
        r.method = QStringLiteral("GET");
        r.path = QStringLiteral("/api/health");
        const Asked a = ask(endpoint, r, 1500);
        CHECK_EQ(a.reply.status, 0);
        CHECK(!a.error.isEmpty());
    }

    SECTION("Local channel — a request reaches the handler as sent, and its answer comes back");
    {
        Server server;
        std::vector<Request> seen;
        QString error;
        const bool up = server.listen(
            endpoint,
            [&seen](const Request& request, Server::Respond respond) {
                seen.push_back(request);
                Reply reply;
                reply.status = 200;
                reply.body = QByteArray("{\"echo\":\"") + request.path.toUtf8() + QByteArray("\"}");
                respond(reply);
            },
            &error);
        CHECK(up);
        if (!up) std::fprintf(stderr, "    listen: %s\n", qPrintable(error));
        CHECK(server.isListening());
        CHECK(server.endpoint() == endpoint);

        Request r;
        r.method = QStringLiteral("POST");
        r.path = QStringLiteral("/api/system/restart");
        r.query = QStringLiteral("p=/admin");
        r.body = QByteArray("{\"x\":1}");
        const Asked a = ask(endpoint, r);
        CHECK_EQ(a.reply.status, 200);
        CHECK(a.reply.body == QByteArray("{\"echo\":\"/api/system/restart\"}"));
        if (a.reply.status == 0) std::fprintf(stderr, "    request: %s\n", qPrintable(a.error));
        CHECK_EQ(static_cast<int>(seen.size()), 1);
        if (seen.size() == 1) {
            CHECK(seen[0].method == QStringLiteral("POST"));
            CHECK(seen[0].path == QStringLiteral("/api/system/restart"));
            CHECK(seen[0].query == QStringLiteral("p=/admin"));
            CHECK(seen[0].body == QByteArray("{\"x\":1}"));
        }

        // The channel is there for the next caller, and the one after.
        for (int i = 0; i < 3; ++i) {
            Request again;
            again.method = QStringLiteral("GET");
            again.path = QStringLiteral("/api/n%1").arg(i);
            const Asked b = ask(endpoint, again);
            CHECK_EQ(b.reply.status, 200);
        }
        CHECK_EQ(static_cast<int>(seen.size()), 4);

        // The name is held: a second server is refused, and says why.
        Server second;
        QString why;
        CHECK(!second.listen(endpoint, [](const Request&, Server::Respond) {}, &why));
        CHECK(!why.isEmpty());
        CHECK(!second.isListening());
        // And the first still answers.
        Request still;
        still.method = QStringLiteral("GET");
        still.path = QStringLiteral("/api/still");
        CHECK_EQ(ask(endpoint, still).reply.status, 200);

        server.close();
        CHECK(!server.isListening());
        const Asked gone = ask(endpoint, still, 1500);
        CHECK_EQ(gone.reply.status, 0);
    }

    SECTION("Local channel — an answer made later is waited for, and a second one is dropped");
    {
        Server server;
        int calls = 0;
        CHECK(server.listen(endpoint, [&calls](const Request&, Server::Respond respond) {
            ++calls;
            QTimer::singleShot(150, qApp, [respond]() {
                Reply first;
                first.status = 201;
                first.body = QByteArray("first");
                respond(first);
                Reply second;
                second.status = 500;
                second.body = QByteArray("second");
                respond(second);
            });
        }));
        Request r;
        r.method = QStringLiteral("GET");
        r.path = QStringLiteral("/api/later");
        const Asked a = ask(endpoint, r);
        CHECK_EQ(a.reply.status, 201);
        CHECK(a.reply.body == QByteArray("first"));
        CHECK_EQ(calls, 1);
    }

    SECTION("Local channel — several callers at once are each answered their own");
    {
        Server server;
        CHECK(server.listen(endpoint, [](const Request& request, Server::Respond respond) {
            // Out of order on purpose: the first asked is the last answered.
            const int n = request.path.mid(QStringLiteral("/api/n").size()).toInt();
            QTimer::singleShot(200 - 40 * n, qApp, [respond, request]() {
                Reply reply;
                reply.status = 200;
                reply.body = request.path.toUtf8();
                respond(reply);
            });
        }));
        const int kCallers = 4;
        std::vector<std::shared_ptr<Asked>> results;
        std::vector<QThread*> callers;
        for (int i = 0; i < kCallers; ++i) {
            auto out = std::make_shared<Asked>();
            results.push_back(out);
            Request r;
            r.method = QStringLiteral("GET");
            r.path = QStringLiteral("/api/n%1").arg(i);
            QThread* t = QThread::create([out, endpoint, r]() {
                out->reply = mw::localchannel::request(endpoint, r, 4000, &out->error);
            });
            callers.push_back(t);
            t->start();
        }
        waitFor([&callers]() {
            for (QThread* t : callers)
                if (!t->isFinished()) return false;
            return true;
        });
        for (int i = 0; i < kCallers; ++i) {
            callers[i]->wait(2000);
            delete callers[i];
            CHECK_EQ(results[i]->reply.status, 200);
            CHECK(results[i]->reply.body == QStringLiteral("/api/n%1").arg(i).toUtf8());
        }
    }

    SECTION("Local channel — a caller that speaks HTTP is cut off, unanswered and unheard");
    {
        Server server;
        int calls = 0;
        CHECK(server.listen(endpoint, [&calls](const Request&, Server::Respond respond) {
            ++calls;
            Reply reply;
            reply.status = 200;
            respond(reply);
        }));
        const RawResult http = sendRaw(
            endpoint, QByteArray("GET /api/admin/token HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
        CHECK(http.connected);
        CHECK(http.closedByServer);
        CHECK(http.received.isEmpty());
        CHECK_EQ(calls, 0);

        // A frame longer than a request may be: cut off before its bytes come.
        const RawResult huge = sendRaw(endpoint, QByteArray(kPreamble) + lengthOnly(0x7fffffff));
        CHECK(huge.connected);
        CHECK(huge.closedByServer);
        CHECK(huge.received.isEmpty());
        CHECK_EQ(calls, 0);

        // The right frame around something that is not a request: told so, in
        // this channel's own shape, and the handler is not called.
        const RawResult junk =
            sendRaw(endpoint, QByteArray(kPreamble) + frame(QByteArray("{\"path\":\"x\"}")), 600);
        CHECK(junk.connected);
        FrameReader reader(false, kMaxReplyBytes);
        CHECK(reader.feed(junk.received) == FrameReader::State::Ready);
        const std::optional<Reply> reply = decodeReply(reader.payload());
        CHECK(reply.has_value());
        if (reply) CHECK_EQ(reply->status, 400);
        CHECK_EQ(calls, 0);

        // And the channel still serves a caller that speaks its language.
        Request r;
        r.method = QStringLiteral("GET");
        r.path = QStringLiteral("/api/health");
        CHECK_EQ(ask(endpoint, r).reply.status, 200);
        CHECK_EQ(calls, 1);
    }

    SECTION("Local channel — an answer too large for a frame becomes a refusal that fits");
    {
        Server server;
        CHECK(server.listen(endpoint, [](const Request&, Server::Respond respond) {
            Reply reply;
            reply.status = 200;
            reply.body = QByteArray(static_cast<int>(kMaxReplyBytes), 'x');
            respond(reply);
        }));
        Request r;
        r.method = QStringLiteral("GET");
        r.path = QStringLiteral("/api/large");
        const Asked a = ask(endpoint, r);
        CHECK_EQ(a.reply.status, 500);
        CHECK(a.reply.body.contains("answer_too_large"));
    }

    SECTION("Local channel — a server that leaves with a caller waiting does not keep it");
    {
        auto server = std::make_unique<Server>();
        CHECK(server->listen(endpoint, [](const Request&, Server::Respond) {
            // Never answers.
        }));
        auto out = std::make_shared<Asked>();
        Request r;
        r.method = QStringLiteral("GET");
        r.path = QStringLiteral("/api/never");
        QThread* caller = QThread::create([out, endpoint, r]() {
            out->reply = mw::localchannel::request(endpoint, r, 8000, &out->error);
        });
        caller->start();
        waitFor([]() { return false; }, 300); // let it get in
        server.reset();
        CHECK(waitFor([caller]() { return caller->isFinished(); }, 6000));
        caller->wait(2000);
        delete caller;
        // Told the server is leaving, or cut off: never left hanging.
        CHECK(out->reply.status == 503 || out->reply.status == 0);
    }
}
