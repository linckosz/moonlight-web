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
#include "server/RequestGuard.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QTimer;

/**
 * The channel between the running server and the other programs of this
 * install on the same machine: the tray icon of a server that runs as a
 * service, the command line, a second launch.
 *
 * It is not a network socket. On Windows it is a named pipe that refuses
 * remote clients, open to the server's own account, and to the people signed
 * in at this PC when the server runs as a service (Audience); elsewhere a Unix
 * socket under the server's data folder, open to the server's account. Whoever
 * reaches it is on this machine by construction, which no address on a TCP
 * connection can say.
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

    // Set by the server's end once the request has come in, never on the wire.
    /// Who asked, as the server's end read it (Windows: the caller's token;
    /// elsewhere: the socket peer's uid).
    LocalCaller caller;
    /// The channel is open to the people signed in at this PC (Audience::SignedIn).
    bool service = false;
    /// Who asked, for the log alone: never part of a decision.
    QString callerLabel;
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

/// Who, besides the server's own account, may open the channel.
enum class Audience
{
    /// Nobody: the server runs in its owner's session, and so do the programs
    /// that ask it (the command line, a second launch).
    OwnAccount,
    /// The people signed in at this PC too: the server runs as a service,
    /// under an account nobody signs in with, and the tray icon that stands
    /// for it runs in theirs.
    SignedIn,
};

/// The audience of a server: the people signed in at the PC only when it was
/// started as a service (@p serviceVar: MW_SERVICE is set) and runs where
/// services run, apart from every desktop (@p sessionZero). Both: the variable
/// alone, left set in a desktop session, widens nothing.
inline Audience audienceFor(bool serviceVar, bool sessionZero)
{
    return serviceVar && sessionZero ? Audience::SignedIn : Audience::OwnAccount;
}

/// Whether this process runs in the session Windows keeps for services, where
/// nobody signs in. False elsewhere than on Windows, and when it cannot be read.
bool runsInSessionZero();

/// Who may open the Windows pipe, as the text its access list is built from,
/// for a server whose account is @p ownerSid. Protected, so nothing is
/// inherited. That account, in full. For Audience::SignedIn, the people signed
/// in at this PC as well, to read and to write data, which is asking and being
/// answered: not to make an instance of the pipe, so the name stays the
/// server's. Nobody else has an entry — not everyone, not every signed-in
/// account of the network, not the anonymous — and the pipe refuses remote
/// clients on top of it.
QString pipeAccessList(const QString& ownerSid, Audience audience);

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
    /// says so, rather than one it shares. @p audience widens the Windows pipe
    /// (pipeAccessList); elsewhere the socket is the server's account's alone
    /// either way.
    bool listen(const QString& endpoint, Handler handler, QString* error = nullptr,
                Audience audience = Audience::OwnAccount);

    /// As listen(); and when the name is held by another program (kNameHeld),
    /// one warning, then another try every @p retryMs on this thread until the
    /// channel opens, with a line when it does. Everything else the server
    /// does goes on meanwhile: the holder only keeps the channel shut.
    bool listenKeepTrying(const QString& endpoint, Handler handler, QString* error = nullptr,
                          Audience audience = Audience::OwnAccount, int retryMs = kRetryMs);
    /// Closes the channel, and stops trying to open it.
    void close();
    bool isListening() const;
    /// Whether it waits for the name to be let go (listenKeepTrying).
    bool isRetrying() const;
    QString endpoint() const;

    /// While the server this channel belongs to starts: every request is
    /// answered earlyReply() on the channel's own thread, without the handler,
    /// so that a launch at the same moment is answered even though the thread
    /// the Server lives on is busy starting and turns no event loop yet.
    /// Asked before listen(), it holds from the first caller on.
    void holdUntilRouted();
    /// From now on, the handler answers.
    void route();
    bool isHeld() const;

    /// The pause between two tries of listenKeepTrying().
    static constexpr int kRetryMs = 30000;

    /// The platform's side of it (LocalChannel.cpp): nothing a caller uses.
    struct Impl;

private:
    void stopRetrying();

    std::shared_ptr<Impl> m_Impl;
    QTimer* m_Retry = nullptr;
    /// Shared with each listen()'s state, read on the channel's thread.
    std::shared_ptr<std::atomic<bool>> m_Early;
};

/// What the channel answers while the server it belongs to is still starting:
/// it is opened first, so that a launch at the same moment finds this server
/// rather than nothing. The same gate as once the server runs, in the same
/// order (verdictOf, then RequestGuard::localChannelGate): 403 to a caller
/// that is not let in, 404 to a route that is not the channel's, and 503
/// "starting" to the rest, with nothing more in it.
Reply earlyReply(const Request& request);

/// What listen() says in its error when the channel's name is held already.
inline constexpr char kNameHeld[] = "the channel's name is already held by another program";

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

/// Whether this caller is let in: RequestGuard::localCallerVerdict with this
/// server's account and the administrators' group as sidText() writes them,
/// on Windows. Elsewhere the socket's peer was judged when it connected
/// (this account or root, read by the kernel), and nobody else got this far.
RequestGuard::LocalVerdict verdictOf(const LocalCaller& caller, bool service);

/// One line per request let in: who asked, and for what, never the body. A
/// caller repeating itself is counted, one line a minute says how many times.
void logAdmitted(const QString& caller, const QString& method, const QString& path);

/// How long the refusals are counted before a line says how many there were.
inline constexpr qint64 kRefusalLineMs = 60 * 1000;

/// The callers the channel turned away: at most one line a minute, saying how
/// many and of which sort. Never which account, process or program.
class RefusalLog
{
public:
    enum class Kind
    {
        StandardAccount, ///< an account that is neither the server's nor an administrator
        Sandboxed,       ///< a low-integrity, AppContainer or restricted token
        Unreadable,      ///< a caller whose token could not be read
        OtherAccount,    ///< a socket peer that is neither this account nor root
    };

    /// @p nowMs: a monotonic clock in milliseconds, the process's when empty.
    explicit RefusalLog(std::function<qint64()> nowMs = {});

    /// Counts one refusal. The line to write now, or empty until a minute has
    /// passed since the last one, the refusals meanwhile being counted.
    QString note(Kind kind);

private:
    std::mutex m_Mutex;
    std::function<qint64()> m_NowMs;
    qint64 m_SaidAt = -1;
    int m_Counts[4] = {};
};

/// The process's refusals, which both ends of the code write to.
RefusalLog& refusals();

// ── A caller's end ──────────────────────────────────────────────────────────

/// What request() says in @p error when a server listens and is another
/// account's: the one failure that asking again does not change.
inline constexpr char kOtherAccount[] = "the channel belongs to another account";

/// One request, waited for at most @p timeoutMs. A reply with status 0 and
/// @p error set when nothing listens, when what listens is not this install's
/// server, or when no answer came in time.
Reply request(const QString& endpoint, const Request& request, int timeoutMs,
              QString* error = nullptr);

#ifdef Q_OS_WIN
/// As request(), and the server's process with it: opened to be waited for
/// (SYNCHRONIZE), from the PID the pipe itself gives, once the pipe is known
/// to be this install's server and before anything is sent — so that what is
/// waited for is the process that was asked, never one that took its PID
/// after it went. @p serverProcess gets a HANDLE the caller closes; it stays
/// null when the reply has status 0.
Reply requestHoldingServer(const QString& endpoint, const Request& request, int timeoutMs,
                           void** serverProcess, QString* error = nullptr);
#endif

// ── Finding the server before starting one ──────────────────────────────────
//
// A launch asks the channel whether this install's server already runs, as a
// service or as this account, before it takes its account's lock: a
// service's lock lives in another profile, and only its channel is common
// to the machine.

/// How far one probe of the channel went.
enum class ProbeStage
{
    NothingListens, ///< no pipe, no socket of that name: the only "nobody"
    NotOpened,      ///< there, but busy past the budget, or refused, or broken
    NotOurs,        ///< opened, and not this install's server: nothing was written
    NoAnswer,       ///< this install's server, and no answer of the channel in time
    Answered,       ///< this install's server answered, whatever its status
};

/// What a launch makes of a probe:
///   - None: nothing listens; it goes on and starts its server;
///   - Found: this install's server answered; it surfaces that one;
///   - Refused: anything else; it starts nothing and says so.
/// Once the server proved to be this install's, the launch never starts a
/// second one, whatever comes after.
enum class ProbeOutcome
{
    None,
    Found,
    Refused,
};

ProbeOutcome probeOutcome(ProbeStage stage);

/// The longest a probe takes, end to end: opening, waiting for the pipe,
/// the server's identity, writing and reading, on one clock.
inline constexpr int kProbeBudgetMs = 2000;

struct Probe
{
    ProbeOutcome outcome = ProbeOutcome::Refused;
    ProbeStage stage = ProbeStage::NotOpened;
    int status = 0; ///< the answer's status, only when the server answered
    bool service =
        false;   ///< what was reached is a service's (the system's pipe, the machine's socket)
    QString why; ///< why it did not, for the log
};

/// The machine's socket of a server that runs as a service (Linux, macOS:
/// /run/moonlightweb/local.sock, /run/moonlightweb-dev/ for the DEV build;
/// the folder is the unit's RuntimeDirectory=); empty on Windows, whose pipe
/// is the machine's already, and for --dev.
QString machineEndpoint(bool devBuild, bool devFlag);

/// The endpoints a caller tries, in order: its account's (@p accountEndpoint),
/// then the machine's when there is one.
QStringList callerEndpoints(const QString& accountEndpoint, bool devBuild, bool devFlag);

/// One request to the first of @p endpoints where something listens: the next
/// one is tried only when nothing listens at this one (ProbeStage::NothingListens),
/// on the same clock.
Reply requestFirst(const QStringList& endpoints, const Request& request, int timeoutMs,
                   QString* error = nullptr);

/// One GET /api/health to the first of @p endpoints where something listens,
/// within @p budgetMs in all: no body, no cookie, nothing else asked.
Probe probe(const QStringList& endpoints, int budgetMs = kProbeBudgetMs);

/// The request probe() sends.
Request healthRequest();

/// What a launch tells its person when it starts nothing, chosen from the
/// probe alone and never from what the server said.
enum class Refusal
{
    None,    ///< nothing to tell: nobody there, or this install's server took the launch
    Refused, ///< another program holds the channel (F4)
    Gate,    ///< this install's server runs, and this account may not ask it (F3)
};

/// Refused for a probe that was refused; Gate for a server found that
/// answered 403, its status read only once it was found; None otherwise.
Refusal refusalFor(const Probe& probe);

/// The two texts, fixed: nothing read from the channel goes into them.
QString refusalText(Refusal refusal);

/// How a refusal is told.
enum class NoticeWay
{
    Tray,   ///< a notification from a short-lived tray icon, then the exit
    Stderr, ///< a line on standard error, nothing to close
};

/// Tray only on the server's own launch path (@p serverLaunch: a launch, the
/// login item, the logon task; never a command-line verb), with a desktop, a
/// notification area and messages it supports. Standard error otherwise, with
/// no box in its place.
NoticeWay refusalNotice(bool serverLaunch, bool hasGui, bool trayAvailable, bool supportsMessages);

/// What a launch does once it asked the channel.
enum class LaunchStep
{
    Start,   ///< go on: take the lock and start the server
    Surface, ///< this install's server runs: surface it, start nothing
    Refuse,  ///< start nothing, and tell why (refusal)
    Stop,    ///< start nothing, and tell nobody: a service or a restart
};

struct LaunchDecision
{
    LaunchStep step = LaunchStep::Refuse;
    Refusal refusal = Refusal::None;
};

/// The launch's way, from the probe alone:
///   - nothing listens → Start;
///   - this install's server found: Stop for a service (@p serviceLaunch) or a
///     restart (@p restart, --after-pid, asked once the server it replaces is
///     gone), so that neither flag ever doubles a server; Refuse with Gate for
///     a 403; Surface otherwise;
///   - refused: Start for a service, which serves without its channel and
///     tries for it again (Server::listenKeepTrying); Refuse otherwise.
LaunchDecision launchDecision(const Probe& probe, bool serviceLaunch, bool restart);

/// How long the notification shows, and the hard cap on the whole wait.
inline constexpr int kNoticeShowMs = 8000;
inline constexpr int kNoticeCapMs = 10000;

} // namespace mw::localchannel
