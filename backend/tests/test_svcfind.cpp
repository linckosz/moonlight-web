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

// A launch finds this install's server before it starts one (server/LocalChannel.h,
// "Finding the server before starting one"): what it makes of a probe, what it
// tells, and the probe and the early channel for real.

#include "server/LocalChannel.h"
#include "test_framework.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using namespace mw::localchannel;

template <class F> bool pumpUntil(F done, int ms)
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

/// A probe from another thread, as a second launch makes it from another
/// process, while this thread plays the server. @p tookMs: how long it took.
Probe probeFrom(const QStringList& endpoints, int budgetMs, qint64* tookMs = nullptr)
{
    auto out = std::make_shared<Probe>();
    auto took = std::make_shared<qint64>(0);
    QThread* caller = QThread::create([out, took, endpoints, budgetMs]() {
        QElapsedTimer t;
        t.start();
        *out = probe(endpoints, budgetMs);
        *took = t.elapsed();
    });
    caller->start();
    pumpUntil([caller]() { return caller->isFinished(); }, budgetMs + 5000);
    caller->wait(2000);
    delete caller;
    if (tookMs) *tookMs = *took;
    return *out;
}

Probe probeAt(ProbeOutcome outcome, ProbeStage stage, int status = 0)
{
    Probe p;
    p.outcome = outcome;
    p.stage = stage;
    p.status = status;
    return p;
}

QString errorOf(const Reply& reply)
{
    return QJsonDocument::fromJson(reply.body).object().value(QStringLiteral("error")).toString();
}

} // namespace

void run_svcfind_tests()
{
    SECTION("SVCFIND — only nothing listening lets a launch start; a server found is never "
            "doubled (S2)");
    {
        CHECK(probeOutcome(ProbeStage::NothingListens) == ProbeOutcome::None);
        CHECK(probeOutcome(ProbeStage::Answered) == ProbeOutcome::Found);
        CHECK(probeOutcome(ProbeStage::NotOpened) == ProbeOutcome::Refused);
        CHECK(probeOutcome(ProbeStage::NotOurs) == ProbeOutcome::Refused);
        CHECK(probeOutcome(ProbeStage::NoAnswer) == ProbeOutcome::Refused);
        // The probe asks one thing, and carries nothing.
        const Request health = healthRequest();
        CHECK(health.method == QStringLiteral("GET"));
        CHECK(health.path == QStringLiteral("/api/health"));
        CHECK(health.query.isEmpty());
        CHECK(health.body.isEmpty());
    }

    SECTION("SVCFIND — F3 is chosen by a status read of a server already found, F4 without "
            "reading anything (SVCN2)");
    {
        // Found + 403 → F3; Found + 200 → none; Found and silent → none;
        // refused → F4, whatever number it carries.
        CHECK(refusalFor(probeAt(ProbeOutcome::Found, ProbeStage::Answered, 403)) == Refusal::Gate);
        CHECK(refusalFor(probeAt(ProbeOutcome::Found, ProbeStage::Answered, 200)) == Refusal::None);
        CHECK(refusalFor(probeAt(ProbeOutcome::Found, ProbeStage::Answered, 503)) == Refusal::None);
        CHECK(refusalFor(probeAt(ProbeOutcome::Found, ProbeStage::NoAnswer, 403)) == Refusal::None);
        CHECK(refusalFor(probeAt(ProbeOutcome::Refused, ProbeStage::NotOurs, 403)) ==
              Refusal::Refused);
        CHECK(refusalFor(probeAt(ProbeOutcome::Refused, ProbeStage::NoAnswer)) == Refusal::Refused);
        CHECK(refusalFor(probeAt(ProbeOutcome::None, ProbeStage::NothingListens)) == Refusal::None);
        // Two fixed texts: no path, account, number or status in them.
        const QString f4 = refusalText(Refusal::Refused);
        const QString f3 = refusalText(Refusal::Gate);
        CHECK(f4.contains(QStringLiteral("another program holds its local channel")));
        CHECK(f3 == QStringLiteral("MoonlightWeb runs as a service on this PC; ask its "
                                   "administrator."));
        CHECK(refusalText(Refusal::None).isEmpty());
        for (const QString& text : {f3, f4}) {
            CHECK(!text.contains(QLatin1Char('\\')) && !text.contains(QLatin1Char('/')));
            CHECK(!text.contains(QStringLiteral("S-1-")));
            CHECK(!text.contains(QStringLiteral("403")));
        }
    }

    SECTION("SVCFIND — a refused launch's log says why, in fixed words only");
    {
        Probe notOurs = probeAt(ProbeOutcome::Refused, ProbeStage::NotOurs);
        notOurs.why = RequestGuard::serverVerdictText(RequestGuard::ServerVerdict::NotSessionZero);
        CHECK(refusedProbeReason(notOurs) ==
              QStringLiteral("the program on the channel is not a service"));
        // Anything but a verdict's own text stays out of the log.
        notOurs.why = QStringLiteral("C:\\Users\\x\\evil.exe S-1-5-21-1-2-3-1001");
        CHECK(refusedProbeReason(notOurs) == QStringLiteral("not this install's server"));
        Probe notOpened = probeAt(ProbeOutcome::Refused, ProbeStage::NotOpened);
        notOpened.why = QStringLiteral("the channel cannot be opened (some system text)");
        CHECK(refusedProbeReason(notOpened) ==
              QStringLiteral("the channel could not be opened in time"));
        CHECK(refusedProbeReason(probeAt(ProbeOutcome::Refused, ProbeStage::NoAnswer)) ==
              QStringLiteral("this install's server did not answer in time"));
        CHECK(
            refusedProbeReason(probeAt(ProbeOutcome::None, ProbeStage::NothingListens)).isEmpty());
        CHECK(
            refusedProbeReason(probeAt(ProbeOutcome::Found, ProbeStage::Answered, 200)).isEmpty());
    }

    SECTION("SVCFIND — a launch's way: a service or a restart never doubles a server, and a "
            "service goes on beside a squatter");
    {
        const Probe none = probeAt(ProbeOutcome::None, ProbeStage::NothingListens);
        const Probe found = probeAt(ProbeOutcome::Found, ProbeStage::Answered, 200);
        const Probe starting = probeAt(ProbeOutcome::Found, ProbeStage::Answered, 503);
        const Probe gate = probeAt(ProbeOutcome::Found, ProbeStage::Answered, 403);
        const Probe refused = probeAt(ProbeOutcome::Refused, ProbeStage::NotOurs);
        for (bool service : {false, true})
            for (bool restart : {false, true})
                CHECK(launchDecision(none, service, restart).step == LaunchStep::Start);
        CHECK(launchDecision(found, false, false).step == LaunchStep::Surface);
        CHECK(launchDecision(starting, false, false).step == LaunchStep::Surface);
        CHECK(launchDecision(gate, false, false).step == LaunchStep::Refuse);
        CHECK(launchDecision(gate, false, false).refusal == Refusal::Gate);
        // --after-pid, once the server it replaces is gone, with a service there:
        // no second server.
        for (const Probe& p : {found, starting, gate}) {
            CHECK(launchDecision(p, false, true).step == LaunchStep::Stop);
            CHECK(launchDecision(p, true, false).step == LaunchStep::Stop);
            CHECK(launchDecision(p, true, true).step == LaunchStep::Stop);
        }
        CHECK(launchDecision(refused, false, false).step == LaunchStep::Refuse);
        CHECK(launchDecision(refused, false, false).refusal == Refusal::Refused);
        CHECK(launchDecision(refused, false, true).step == LaunchStep::Refuse);
        CHECK(launchDecision(refused, true, false).step == LaunchStep::Start);
    }

    SECTION("SVCFIND — the refusal is a notification only on the server's launch path, with a "
            "tray that takes messages; never a box (N1)");
    {
        CHECK(refusalNotice(true, true, true, true) == NoticeWay::Tray);
        // A command-line verb (--local-request, --quit, the installer's quit,
        // --worker-*, --update-apply…) never shows one, whatever the desktop.
        CHECK(refusalNotice(false, true, true, true) == NoticeWay::Stderr);
        for (int bits = 0; bits < 8; ++bits) {
            const bool gui = (bits & 1) != 0, tray = (bits & 2) != 0, msgs = (bits & 4) != 0;
            if (gui && tray && msgs) continue;
            CHECK(refusalNotice(true, gui, tray, msgs) == NoticeWay::Stderr);
            CHECK(refusalNotice(false, gui, tray, msgs) == NoticeWay::Stderr);
        }
        // Bounded: the notification's time, then a hard cap above it.
        CHECK(kNoticeShowMs == 8000);
        CHECK(kNoticeCapMs == 10000);
        CHECK(kNoticeCapMs > kNoticeShowMs);
    }

    SECTION("SVCFIND — the endpoints a caller tries: its account's, then a service's machine "
            "socket (S5)");
    {
#ifdef Q_OS_WIN
        CHECK(machineEndpoint(false, false).isEmpty());
        CHECK(machineEndpoint(true, false).isEmpty());
        CHECK(callerEndpoints(QStringLiteral("\\\\.\\pipe\\x"), false, false) ==
              QStringList{QStringLiteral("\\\\.\\pipe\\x")});
#else
        CHECK(machineEndpoint(false, false) == QStringLiteral("/run/moonlightweb/local.sock"));
        // The DEV build's package, and so its unit's folder, have a name of their own.
        CHECK(machineEndpoint(true, false) == QStringLiteral("/run/moonlightweb-dev/local.sock"));
        const QString mine = QStringLiteral("/home/a/.local/share/mw/channel/local.sock");
        CHECK(callerEndpoints(mine, false, false) ==
              (QStringList{mine, QStringLiteral("/run/moonlightweb/local.sock")}));
#endif
        // A --dev run never uses the machine's.
        CHECK(machineEndpoint(false, true).isEmpty());
        CHECK(machineEndpoint(true, true).isEmpty());
        CHECK(callerEndpoints(QStringLiteral("x"), false, true) ==
              QStringList{QStringLiteral("x")});
    }

#ifndef Q_OS_WIN
    SECTION("SVCFIND — on the machine's socket, the gate lets in this account and root alone "
            "(S5)");
    {
        LocalCaller other;
        other.uid = static_cast<qint64>(::geteuid()) + 1;
        if (other.uid == 0) other.uid = 2;
        LocalCaller self;
        self.uid = static_cast<qint64>(::geteuid());
        LocalCaller root;
        root.uid = 0;
        LocalCaller unread;
        unread.uid = -1;
        CHECK(verdictOf(self, true) == RequestGuard::LocalVerdict::Admit);
        CHECK(verdictOf(root, true) == RequestGuard::LocalVerdict::Admit);
        CHECK(verdictOf(other, true) == RequestGuard::LocalVerdict::StandardAccount);
        CHECK(verdictOf(unread, true) == RequestGuard::LocalVerdict::Unreadable);
        // This account's socket: the kernel judged the peer at connect.
        CHECK(verdictOf(other, false) == RequestGuard::LocalVerdict::Admit);
        // And a refused caller is answered 403 whatever it asks, even early.
        Request r = healthRequest();
        r.caller = other;
        r.service = true;
        CHECK_EQ(earlyReply(r).status, 403);
    }

    SECTION("SVCFIND — a machine socket is made only in a folder nobody else can have made "
            "(S5)");
    {
        // A temporary folder sits under /tmp, which anybody writes: refused.
        QTemporaryDir dir;
        Server server;
        QString why;
        CHECK(!server.listen(
            dir.path() + QStringLiteral("/local.sock"), [](const Request&, Server::Respond) {},
            &why, Audience::SignedIn));
        CHECK(why.contains(QStringLiteral("not this service's alone")));
        CHECK(!server.isListening());
    }
#endif

    // ── For real ────────────────────────────────────────────────────────────
    QTemporaryDir data;
    const QString endpoint = endpointFor(
        QStringLiteral("mw-svcfind-%1-local").arg(QCoreApplication::applicationPid()), data.path());
    const QStringList endpoints{endpoint};

    SECTION("SVCFIND — nothing listens: None, at once");
    {
        qint64 took = 0;
        const Probe p = probeFrom(endpoints, kProbeBudgetMs, &took);
        CHECK(p.outcome == ProbeOutcome::None);
        CHECK(p.stage == ProbeStage::NothingListens);
        CHECK(took < 1000);
    }

    SECTION("SVCFIND — this install's server answers: Found, with its status");
    {
        Server server;
        CHECK(server.listen(endpoint, [](const Request& request, Server::Respond respond) {
            Reply reply;
            reply.status = request.path == QStringLiteral("/api/health") ? 200 : 404;
            respond(reply);
        }));
        const Probe p = probeFrom(endpoints, kProbeBudgetMs);
        CHECK(p.outcome == ProbeOutcome::Found);
        CHECK_EQ(p.status, 200);
        CHECK(!p.service);
        if (p.outcome != ProbeOutcome::Found)
            std::fprintf(stderr, "    probe: %s\n", qPrintable(p.why));
    }

    SECTION("SVCFIND — a server that never answers is Refused within the probe's 2 s (S2, S3)");
    {
        Server server;
        CHECK(server.listen(endpoint, [](const Request&, Server::Respond) {
            // Never answers.
        }));
        qint64 took = 0;
        const Probe p = probeFrom(endpoints, kProbeBudgetMs, &took);
        CHECK(p.outcome == ProbeOutcome::Refused);
        CHECK(p.stage == ProbeStage::NoAnswer);
        CHECK(took >= kProbeBudgetMs - 200);
        CHECK(took <= kProbeBudgetMs + 500);
        CHECK(refusalFor(p) == Refusal::Refused);
    }

    SECTION("SVCFIND — while starting, the channel answers 503 behind the same gate, and says "
            "nothing more (B1)");
    {
        // The server's thread stays BLOCKED, as main's is while it starts:
        // the early answer comes from the channel's own thread, in time.
        Server server;
        server.holdUntilRouted();
        CHECK(server.isHeld());
        CHECK(server.listen(endpoint, [](const Request&, Server::Respond respond) {
            Reply reply;
            reply.status = 200;
            respond(reply);
        }));
        auto blocked = std::make_shared<Probe>();
        auto took = std::make_shared<qint64>(0);
        QThread* caller = QThread::create([blocked, took, endpoints]() {
            QElapsedTimer t;
            t.start();
            *blocked = probe(endpoints, kProbeBudgetMs);
            *took = t.elapsed();
        });
        caller->start();
        QThread::msleep(5000); // no event turns on this thread meanwhile
        CHECK(caller->wait(5000));
        delete caller;
        CHECK(blocked->outcome == ProbeOutcome::Found);
        CHECK_EQ(blocked->status, 503);
        CHECK(*took < kProbeBudgetMs);
        CHECK(refusalFor(*blocked) == Refusal::None);
        if (blocked->outcome != ProbeOutcome::Found)
            std::fprintf(stderr, "    blocked probe: %s\n", qPrintable(blocked->why));

        // Routed: the handler answers from then on.
        server.route();
        CHECK(!server.isHeld());
        const Probe routed = probeFrom(endpoints, kProbeBudgetMs);
        CHECK(routed.outcome == ProbeOutcome::Found);
        CHECK_EQ(routed.status, 200);

        // Asked of the early answer directly, with a caller the gate lets in
        // (this account: what the channel read of the probe above).
        Request asked = healthRequest();
#ifdef Q_OS_WIN
        asked.caller.readable = false; // nothing read → refused before anything else
        CHECK_EQ(earlyReply(asked).status, 403);
        CHECK(errorOf(earlyReply(asked)) == QStringLiteral("forbidden"));
#else
        asked.caller.uid = static_cast<qint64>(::geteuid());
        const Reply starting = earlyReply(asked);
        CHECK_EQ(starting.status, 503);
        CHECK(starting.body == QByteArray("{\"error\":\"starting\"}"));
        asked.path = QStringLiteral("/api/not-a-route");
        CHECK_EQ(earlyReply(asked).status, 404);
#endif
    }

    SECTION("SVCFIND — a start that fails closes its early channel, and the next launch finds "
            "nothing (B3)");
    {
        {
            Server early;
            early.holdUntilRouted();
            CHECK(early.listen(endpoint, [](const Request&, Server::Respond) {}));
            CHECK(probeFrom(endpoints, kProbeBudgetMs).outcome == ProbeOutcome::Found);
            // The start fails here: the channel's owner goes out of scope.
        }
        const Probe next = probeFrom(endpoints, kProbeBudgetMs);
        CHECK(next.outcome == ProbeOutcome::None);
        CHECK(launchDecision(next, false, false).step == LaunchStep::Start);
    }

    SECTION("SVCFIND — a name held by another program: one warning, another try later, and the "
            "channel once it is let go (S4)");
    {
        auto holder = std::make_unique<Server>();
        CHECK(holder->listen(endpoint, [](const Request&, Server::Respond) {}));
        Server server;
        QString why;
        const bool open = server.listenKeepTrying(
            endpoint,
            [](const Request&, Server::Respond respond) {
                Reply reply;
                reply.status = 200;
                respond(reply);
            },
            &why, Audience::OwnAccount, 200);
        CHECK(!open);
        CHECK(why == QLatin1String(kNameHeld));
        CHECK(server.isRetrying());
        CHECK(!server.isListening());
        pumpUntil([]() { return false; }, 500);
        CHECK(!server.isListening()); // still held
        holder.reset();
        CHECK(pumpUntil([&server]() { return server.isListening(); }, 3000));
        CHECK(!server.isRetrying());
        const Probe p = probeFrom(endpoints, kProbeBudgetMs);
        CHECK(p.outcome == ProbeOutcome::Found);
        CHECK_EQ(p.status, 200);
        // Closing stops the tries as well.
        server.close();
        CHECK(!server.isRetrying());
    }

    SECTION("SVCFIND — any other failure to listen is not retried");
    {
        Server server;
        QString why;
        CHECK(!server.listenKeepTrying(QString(), {}, &why));
        CHECK(!server.isRetrying());
    }

#ifdef Q_OS_WIN
    SECTION("SVCFIND — the channel opened early is made with the access list a late one had "
            "(B2)");
    {
        for (Audience audience : {Audience::OwnAccount, Audience::SignedIn}) {
            Server server;
            CHECK(server.listenKeepTrying(
                endpoint, [](const Request&, Server::Respond) {}, nullptr, audience));
            HANDLE pipe = ::CreateFileW(endpoint.toStdWString().c_str(),
                                        GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING,
                                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            CHECK(pipe != INVALID_HANDLE_VALUE);
            if (pipe == INVALID_HANDLE_VALUE) continue;
            PSID owner = nullptr;
            PACL dacl = nullptr;
            PSECURITY_DESCRIPTOR sd = nullptr;
            const DWORD got = ::GetSecurityInfo(
                pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &owner, nullptr, &dacl, nullptr, &sd);
            CHECK(got == ERROR_SUCCESS);
            QString ownerText;
            QStringList trustees;
            if (got == ERROR_SUCCESS) {
                LPWSTR text = nullptr;
                if (::ConvertSidToStringSidW(owner, &text)) {
                    ownerText = QString::fromWCharArray(text);
                    ::LocalFree(text);
                }
                for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
                    void* ace = nullptr;
                    if (!::GetAce(dacl, i, &ace)) continue;
                    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
                    CHECK(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
                    if (::ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart),
                                                 &text)) {
                        trustees.append(QString::fromWCharArray(text));
                        ::LocalFree(text);
                    }
                }
                ::LocalFree(sd);
            }
            ::CloseHandle(pipe);
            // What pipeAccessList() states for this account: its owner, then
            // the account alone, or the account and who is signed in.
            CHECK(!ownerText.isEmpty());
            const QString list = pipeAccessList(ownerText, audience);
            CHECK(list.startsWith(QStringLiteral("O:") + ownerText + QStringLiteral("D:P(")));
            const QStringList expected =
                audience == Audience::OwnAccount
                    ? QStringList{ownerText}
                    : QStringList{ownerText, QStringLiteral("S-1-5-4")}; // IU
            CHECK(trustees == expected);
        }
    }
#else
    SECTION("SVCFIND — a socket a dead server left is nobody; one replaced by a link stops "
            "(stale socket)");
    {
        // A socket file with nobody behind it, in the folder the channel keeps.
        {
            Server made;
            CHECK(made.listen(endpoint, [](const Request&, Server::Respond) {}));
        }
        const QByteArray path = endpoint.toLocal8Bit();
        ::unlink(path.constData());
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.constData());
        CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        ::close(fd); // bound, never listened on: what a dead server leaves
        struct stat info = {};
        CHECK(::lstat(path.constData(), &info) == 0 && S_ISSOCK(info.st_mode));

        const Probe dead = probeFrom(endpoints, kProbeBudgetMs);
        CHECK(dead.outcome == ProbeOutcome::None);
        // Taken over by the next server, after its lock (listen removes it).
        {
            Server next;
            CHECK(next.listen(endpoint, [](const Request&, Server::Respond respond) {
                Reply reply;
                reply.status = 200;
                respond(reply);
            }));
            CHECK(probeFrom(endpoints, kProbeBudgetMs).outcome == ProbeOutcome::Found);
        }

        // A link in its place: neither nobody nor this install's server.
        ::unlink(path.constData());
        const QByteArray target = (data.path() + QStringLiteral("/elsewhere")).toLocal8Bit();
        CHECK(::symlink(target.constData(), path.constData()) == 0);
        const Probe linked = probeFrom(endpoints, kProbeBudgetMs);
        CHECK(linked.outcome == ProbeOutcome::Refused);
        CHECK(launchDecision(linked, false, false).step == LaunchStep::Refuse);
        Server server;
        QString why;
        CHECK(!server.listenKeepTrying(endpoint, [](const Request&, Server::Respond) {}, &why));
        CHECK(!server.isListening());
        CHECK(!server.isRetrying());
        CHECK(::lstat(path.constData(), &info) == 0 && S_ISLNK(info.st_mode)); // left alone
        ::unlink(path.constData());
    }
#endif
}
