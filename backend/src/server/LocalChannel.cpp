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

#include "server/LocalChannel.h"

#include "common/Logger.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <initializer_list>
#include <mutex>
#include <utility>

#ifdef Q_OS_WIN
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>

#include <cerrno>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mw::localchannel {

namespace {

/// How long a caller has to send its request once connected.
constexpr int kRequestReadMs = 5000;
/// How long the server waits for its own answer before giving the caller one:
/// above the longest wait a command-line verb allows itself (three minutes,
/// to turn the internet link on).
constexpr int kHandlerMs = 200000;

bool hasControlCharacter(const QString& text)
{
    for (const QChar c : text)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) return true;
    return false;
}

/// Whether @p obj holds nothing but @p allowed: a field this channel does not
/// know is a request it does not understand, and is refused whole.
bool onlyKnownFields(const QJsonObject& obj, std::initializer_list<QLatin1String> allowed)
{
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        bool known = false;
        for (const QLatin1String name : allowed)
            known = known || it.key() == name;
        if (!known) return false;
    }
    return true;
}

} // namespace

// ── The wire ────────────────────────────────────────────────────────────────

QByteArray frame(const QByteArray& payload)
{
    const quint32 n = static_cast<quint32>(payload.size());
    QByteArray out;
    out.reserve(payload.size() + 4);
    out.append(static_cast<char>((n >> 24) & 0xff));
    out.append(static_cast<char>((n >> 16) & 0xff));
    out.append(static_cast<char>((n >> 8) & 0xff));
    out.append(static_cast<char>(n & 0xff));
    out.append(payload);
    return out;
}

QByteArray encodeRequest(const Request& request)
{
    QJsonObject obj;
    obj[QStringLiteral("method")] = request.method;
    obj[QStringLiteral("path")] = request.path;
    if (!request.query.isEmpty()) obj[QStringLiteral("query")] = request.query;
    if (!request.body.isEmpty())
        obj[QStringLiteral("body")] = QString::fromLatin1(request.body.toBase64());
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

std::optional<Request> decodeRequest(const QByteArray& payload)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    const QJsonObject obj = doc.object();
    if (!onlyKnownFields(obj, {QLatin1String("method"), QLatin1String("path"),
                               QLatin1String("query"), QLatin1String("body")}))
        return std::nullopt;
    if (!obj.value(QStringLiteral("method")).isString() ||
        !obj.value(QStringLiteral("path")).isString())
        return std::nullopt;
    const QJsonValue query = obj.value(QStringLiteral("query"));
    if (!query.isUndefined() && !query.isString()) return std::nullopt;

    Request out;
    out.method = obj.value(QStringLiteral("method")).toString();
    out.path = obj.value(QStringLiteral("path")).toString();
    out.query = query.toString();

    static const QRegularExpression verb(QStringLiteral("^[A-Z]{1,8}$"));
    if (!verb.match(out.method).hasMatch()) return std::nullopt;
    // A path of this server and nothing else: from the root, with no scheme, no
    // host and no query. It is handed to the server's own router, never fetched.
    if (!out.path.startsWith(QLatin1Char('/')) || out.path.startsWith(QLatin1String("//")) ||
        out.path.size() > 2048)
        return std::nullopt;
    if (out.path.contains(QLatin1Char('?')) || out.path.contains(QLatin1Char('#')) ||
        out.path.contains(QLatin1Char('\\')) || out.path.contains(QLatin1String("://")))
        return std::nullopt;
    if (out.query.size() > 4096 || out.query.contains(QLatin1Char('#'))) return std::nullopt;
    if (hasControlCharacter(out.path) || hasControlCharacter(out.query)) return std::nullopt;

    const QJsonValue body = obj.value(QStringLiteral("body"));
    if (!body.isUndefined()) {
        if (!body.isString()) return std::nullopt;
        const auto decoded = QByteArray::fromBase64Encoding(
            body.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) return std::nullopt;
        out.body = decoded.decoded;
    }
    return out;
}

QByteArray encodeReply(const Reply& reply)
{
    QJsonObject obj;
    obj[QStringLiteral("status")] = reply.status;
    if (!reply.body.isEmpty())
        obj[QStringLiteral("body")] = QString::fromLatin1(reply.body.toBase64());
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

std::optional<Reply> decodeReply(const QByteArray& payload)
{
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    const QJsonObject obj = doc.object();
    if (!onlyKnownFields(obj, {QLatin1String("status"), QLatin1String("body")}))
        return std::nullopt;

    Reply out;
    out.status = obj.value(QStringLiteral("status")).toInt(0);
    if (out.status < 100 || out.status > 599) return std::nullopt;
    const QJsonValue body = obj.value(QStringLiteral("body"));
    if (!body.isUndefined()) {
        if (!body.isString()) return std::nullopt;
        const auto decoded = QByteArray::fromBase64Encoding(
            body.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok) return std::nullopt;
        out.body = decoded.decoded;
    }
    return out;
}

FrameReader::FrameReader(bool expectPreamble, quint32 maxBytes)
    : m_ExpectPreamble(expectPreamble)
    , m_MaxBytes(maxBytes)
{}

FrameReader::State FrameReader::feed(const QByteArray& bytes)
{
    if (m_State == State::Bad) return m_State;
    if (m_State == State::Ready) {
        // One request, one answer: nothing follows a frame.
        if (!bytes.isEmpty()) {
            m_State = State::Bad;
            m_Error = QStringLiteral("bytes after the frame");
        }
        return m_State;
    }
    m_Buffer.append(bytes);
    m_State = parse();
    return m_State;
}

FrameReader::State FrameReader::parse()
{
    const qsizetype lead = m_ExpectPreamble ? static_cast<qsizetype>(sizeof(kPreamble) - 1) : 0;
    if (lead > 0) {
        // Refused at the first byte that differs, without waiting for the rest:
        // an HTTP request line is turned away as it starts.
        const qsizetype have = qMin(lead, m_Buffer.size());
        if (m_Buffer.left(have) != QByteArray(kPreamble, static_cast<int>(have))) {
            m_Error = QStringLiteral("not this channel's opening line");
            return State::Bad;
        }
    }
    if (m_Buffer.size() < lead + 4) return State::NeedMore;

    const auto* p = reinterpret_cast<const unsigned char*>(m_Buffer.constData() + lead);
    const quint32 length = (static_cast<quint32>(p[0]) << 24) | (static_cast<quint32>(p[1]) << 16) |
                           (static_cast<quint32>(p[2]) << 8) | static_cast<quint32>(p[3]);
    if (length == 0 || length > m_MaxBytes) {
        m_Error = QStringLiteral("frame of %1 bytes, %2 at most").arg(length).arg(m_MaxBytes);
        return State::Bad;
    }
    const qsizetype total = lead + 4 + static_cast<qsizetype>(length);
    if (m_Buffer.size() < total) return State::NeedMore;
    if (m_Buffer.size() > total) {
        m_Error = QStringLiteral("bytes after the frame");
        return State::Bad;
    }
    m_Payload = m_Buffer.mid(lead + 4, static_cast<qsizetype>(length));
    m_Buffer.clear();
    return State::Ready;
}

// ── Where it is ─────────────────────────────────────────────────────────────

QString channelName(const QString& productName, bool devFlag)
{
    return productName + (devFlag ? QStringLiteral("-dev") : QString()) + QStringLiteral("-local");
}

QString endpointFor(const QString& channelName, const QString& dataDir)
{
#ifdef Q_OS_WIN
    Q_UNUSED(dataDir);
    return QStringLiteral("\\\\.\\pipe\\") + channelName;
#else
    Q_UNUSED(channelName);
    // A folder of its own, which the server keeps to its account alone: the
    // data folder itself is often readable by others.
    return dataDir + QStringLiteral("/channel/local.sock");
#endif
}

bool runsInSessionZero()
{
#ifdef Q_OS_WIN
    DWORD session = 0;
    return ::ProcessIdToSessionId(::GetCurrentProcessId(), &session) != FALSE && session == 0;
#else
    return false;
#endif
}

QString pipeAccessList(const QString& ownerSid, Audience audience)
{
    const QString own = QStringLiteral("O:%1D:P(A;;GA;;;%1)").arg(ownerSid);
    if (audience == Audience::OwnAccount) return own;
    // 0x12008b: FILE_GENERIC_READ | FILE_WRITE_DATA. FILE_GENERIC_WRITE would
    // add FILE_APPEND_DATA, which on a pipe is the right to create an instance.
    return own + QStringLiteral("(A;;0x12008b;;;IU)");
}

// ── Into the server ─────────────────────────────────────────────────────────

HttpRequest httpRequestFor(const Request& request)
{
    HttpRequest out;
    out.method = request.method;
    out.path = request.path;
    const QUrlQuery query(request.query);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto& item : items)
        out.queryParams.insert(item.first, item.second);
    out.body = request.body;
    // The only headers there are, and both are this code's own.
    if (!request.body.isEmpty())
        out.headers.insert(QStringLiteral("content-type"), QStringLiteral("application/json"));
    out.headers.insert(QStringLiteral("content-length"), QString::number(request.body.size()));
    out.clientAddress = QLatin1String(kClientLabel);
    // What the server's end read, never anything of the wire.
    out.localCaller = request.caller;
    out.localChannelService = request.service;
    out.localCallerLabel = request.callerLabel;
    return out;
}

Reply replyFor(const HttpResponse& response)
{
    Reply out;
    out.status = response.statusCode;
    out.body = response.body;
    return out;
}

RefusalLog::RefusalLog(std::function<qint64()> nowMs)
    : m_NowMs(std::move(nowMs))
{
    if (!m_NowMs) {
        m_NowMs = []() {
            static const QElapsedTimer since = []() {
                QElapsedTimer timer;
                timer.start();
                return timer;
            }();
            return since.elapsed();
        };
    }
}

QString RefusalLog::note(Kind kind)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ++m_Counts[static_cast<int>(kind)];
    const qint64 now = m_NowMs();
    if (m_SaidAt >= 0 && now - m_SaidAt < kRefusalLineMs) return {};

    static const char* const kNames[] = {"standard account", "sandboxed", "unreadable",
                                         "another account"};
    int total = 0;
    QStringList sorts;
    for (int i = 0; i < 4; ++i) {
        if (m_Counts[i] == 0) continue;
        total += m_Counts[i];
        sorts << QStringLiteral("%1 %2").arg(m_Counts[i]).arg(QLatin1String(kNames[i]));
        m_Counts[i] = 0;
    }
    m_SaidAt = now;
    return QStringLiteral("[LocalChannel] %1 caller(s) turned away since the last such line (%2)")
        .arg(total)
        .arg(sorts.join(QStringLiteral(", ")));
}

RefusalLog& refusals()
{
    static RefusalLog log;
    return log;
}

void logAdmitted(const QString& caller, const QString& method, const QString& path)
{
    struct Seen
    {
        qint64 loggedAt = 0;
        int since = 0;
    };
    static std::mutex mutex;
    static QHash<QString, Seen> seen;
    static QElapsedTimer clock;

    QString repeats;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!clock.isValid()) clock.start();
        if (seen.size() > 256) seen.clear();
        Seen& s = seen[caller + QLatin1Char('|') + method + QLatin1Char('|') + path];
        const qint64 now = clock.elapsed();
        if (s.loggedAt != 0 && now - s.loggedAt < 60000) {
            ++s.since;
            return;
        }
        if (s.since > 0)
            repeats = QStringLiteral(" (and %1 more since the last line)").arg(s.since);
        s.loggedAt = now + 1; // never 0: 0 means "not yet written"
        s.since = 0;
    }
    Logger::info(
        QStringLiteral("[LocalChannel] %1 %2 from %3%4").arg(method, path, caller, repeats));
}

namespace {

/// The answer a caller gets when the server could not make one.
Reply refusal(int status, const char* why)
{
    Reply out;
    out.status = status;
    out.body = QJsonDocument(QJsonObject{{QStringLiteral("error"), QLatin1String(why)}})
                   .toJson(QJsonDocument::Compact);
    return out;
}

/// An answer as it goes on the wire: one too large for a frame becomes a
/// refusal that fits.
QByteArray replyFrame(const Reply& reply)
{
    QByteArray payload = encodeReply(reply);
    if (static_cast<quint32>(payload.size()) > kMaxReplyBytes)
        payload = encodeReply(refusal(500, "answer_too_large"));
    return frame(payload);
}

} // namespace

#ifdef Q_OS_WIN

// ═════════════════════════════════════════════════════════════════════════════
// Windows: a named pipe
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// The most callers served at once; one more is turned away.
constexpr int kMaxConnections = 16;

QString lastErrorText(DWORD code)
{
    return QStringLiteral("error %1").arg(code);
}

/// One overlapped read or write, given up after @p timeoutMs.
bool pipeIo(HANDLE pipe, bool write, void* data, DWORD size, DWORD* done, DWORD timeoutMs)
{
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    *done = 0;
    BOOL ok = write ? ::WriteFile(pipe, data, size, nullptr, &ov)
                    : ::ReadFile(pipe, data, size, nullptr, &ov);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        if (::WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
            ok = ::GetOverlappedResult(pipe, &ov, done, FALSE);
        } else {
            ::CancelIoEx(pipe, &ov);
            ::GetOverlappedResult(pipe, &ov, done, TRUE);
            ok = FALSE;
        }
    } else if (ok) {
        ok = ::GetOverlappedResult(pipe, &ov, done, TRUE);
    }
    ::CloseHandle(ov.hEvent);
    return ok != 0;
}

bool writeAll(HANDLE pipe, const QByteArray& bytes, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    qsizetype sent = 0;
    while (sent < bytes.size()) {
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0) return false;
        DWORD done = 0;
        if (!pipeIo(pipe, true, const_cast<char*>(bytes.constData() + sent),
                    static_cast<DWORD>(bytes.size() - sent), &done, static_cast<DWORD>(left)) ||
            done == 0)
            return false;
        sent += done;
    }
    return true;
}

/// Reads until @p reader has its frame, refuses, or @p timeoutMs pass.
bool readFrame(HANDLE pipe, FrameReader& reader, int timeoutMs, QString* why)
{
    QElapsedTimer clock;
    clock.start();
    char buffer[16 * 1024];
    while (reader.state() == FrameReader::State::NeedMore) {
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0) {
            *why = QStringLiteral("timed out");
            return false;
        }
        DWORD got = 0;
        if (!pipeIo(pipe, false, buffer, static_cast<DWORD>(sizeof(buffer)), &got,
                    static_cast<DWORD>(left)) ||
            got == 0) {
            *why = QStringLiteral("the other end closed or went silent");
            return false;
        }
        reader.feed(QByteArray(buffer, static_cast<int>(got)));
    }
    if (reader.state() == FrameReader::State::Bad) {
        *why = reader.error();
        return false;
    }
    return true;
}

static_assert(RequestGuard::kGroupEnabled == SE_GROUP_ENABLED);
static_assert(RequestGuard::kGroupDenyOnly == SE_GROUP_USE_FOR_DENY_ONLY);
static_assert(RequestGuard::kMediumIntegrity == SECURITY_MANDATORY_MEDIUM_RID);

/// A SID as text. The one writer of every SID the channel compares, so that
/// two of them are equal exactly when the SIDs are; empty when it fails.
QString sidText(PSID sid)
{
    if (!sid || !::IsValidSid(sid)) return {};
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(sid, &text)) return {};
    QString out = QString::fromWCharArray(text);
    ::LocalFree(text);
    return out;
}

/// One class of information of @p token; empty when it cannot be read.
std::vector<unsigned char> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS what)
{
    DWORD size = 0;
    ::GetTokenInformation(token, what, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<unsigned char> out(size);
    if (!::GetTokenInformation(token, what, out.data(), size, &size)) return {};
    return out;
}

/// This process's account, as sidText() writes it.
QString accountSid()
{
    static const QString own = []() {
        QString out;
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
        const std::vector<unsigned char> user = tokenInfo(token, TokenUser);
        if (!user.empty())
            out = sidText(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid);
        ::CloseHandle(token);
        return out;
    }();
    return own;
}

/// BUILTIN\Administrators, as sidText() writes it.
QString administratorsSid()
{
    static const QString admins = []() {
        BYTE sid[SECURITY_MAX_SID_SIZE];
        DWORD size = sizeof(sid);
        if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &size))
            return QString();
        return sidText(sid);
    }();
    return admins;
}

std::wstring ownSid()
{
    return accountSid().toStdWString();
}

/// The caller's identity on this thread, for as long as it takes to open its
/// token, and never longer: end() lets it go, and so does leaving the scope.
class Impersonation
{
public:
    explicit Impersonation(HANDLE pipe)
        : m_Active(::ImpersonateNamedPipeClient(pipe) != 0)
    {}
    ~Impersonation()
    {
        if (m_Active) ::RevertToSelf();
    }
    Impersonation(const Impersonation&) = delete;
    Impersonation& operator=(const Impersonation&) = delete;

    bool active() const { return m_Active; }
    /// False when this thread could not be given back its own identity.
    bool end()
    {
        if (m_Active && ::RevertToSelf()) m_Active = false;
        return !m_Active;
    }

private:
    bool m_Active;
};

enum class CallerRead
{
    Read,
    Unreadable,
    /// This thread still bears the caller's identity: nothing more is done on it.
    NotReverted,
};

/// Who is at the other end of a connected instance, from the caller's own
/// token: the pipe lends it to this thread (the caller opens it at the
/// identification level, enough to read and never to act), the token is
/// opened as this process, and the thread is given back its own identity
/// before anything is read. Nothing of the process ID, nothing the caller wrote.
/// A pipe lends its caller's token only once something was read from it.
CallerRead readCaller(HANDLE pipe, LocalCaller& out)
{
    out = LocalCaller{};
    HANDLE token = nullptr;
    {
        Impersonation as(pipe);
        if (!as.active()) return CallerRead::Unreadable;
        const BOOL opened = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
        if (!as.end()) {
            if (opened) ::CloseHandle(token);
            return CallerRead::NotReverted;
        }
        if (!opened) return CallerRead::Unreadable;
    }

    bool ok = true;
    LocalCaller read;
    const std::vector<unsigned char> user = tokenInfo(token, TokenUser);
    if (!user.empty())
        read.sid = sidText(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid);
    ok = ok && !read.sid.isEmpty();

    const std::vector<unsigned char> groups = tokenInfo(token, TokenGroups);
    if (ok && !groups.empty()) {
        const auto* list = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
        for (DWORD i = 0; i < list->GroupCount; ++i) {
            const QString sid = sidText(list->Groups[i].Sid);
            if (sid.isEmpty()) {
                ok = false;
                break;
            }
            read.groups.append(TokenGroup{sid, list->Groups[i].Attributes});
        }
    } else {
        ok = false;
    }

    const std::vector<unsigned char> level = tokenInfo(token, TokenIntegrityLevel);
    if (ok && !level.empty()) {
        const PSID label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(level.data())->Label.Sid;
        if (label && ::IsValidSid(label) && *::GetSidSubAuthorityCount(label) > 0)
            read.integrityRid = *::GetSidSubAuthority(label, *::GetSidSubAuthorityCount(label) - 1);
        else
            ok = false;
    } else {
        ok = false;
    }

    const std::vector<unsigned char> container = tokenInfo(token, TokenIsAppContainer);
    if (ok && container.size() >= sizeof(DWORD))
        read.appContainer = *reinterpret_cast<const DWORD*>(container.data()) != 0;
    else
        ok = false;

    if (ok) {
        ::SetLastError(ERROR_SUCCESS);
        const BOOL restricted = ::IsTokenRestricted(token);
        if (!restricted && ::GetLastError() != ERROR_SUCCESS)
            ok = false;
        else
            read.restricted = restricted != 0;
    }
    ::CloseHandle(token);

    if (!ok) return CallerRead::Unreadable;
    read.readable = true;
    out = read;
    return CallerRead::Read;
}

QString imageOf(DWORD pid)
{
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    QString image;
    if (::QueryFullProcessImageNameW(process, 0, path, &size))
        image = QString::fromWCharArray(path, static_cast<int>(size));
    ::CloseHandle(process);
    return image;
}

/// Who is at the other end of a connected instance, for the log: read from the
/// pipe, never from what the caller wrote.
QString callerOf(HANDLE pipe)
{
    ULONG pid = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &pid)) return QStringLiteral("an unknown caller");
    const QString image = imageOf(pid);
    return QStringLiteral("pid %1 (%2)")
        .arg(pid)
        .arg(image.isEmpty() ? QStringLiteral("image not readable") : image);
}

/// Who owns the pipe behind @p pipe: this account (Mine), the system or an
/// administrator (Trusted), somebody else, or nobody that can be read.
enum class PipeOwner
{
    Unreadable,
    Mine,
    Trusted,
    Other,
};

PipeOwner ownerOf(HANDLE pipe)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                          nullptr, nullptr, &sd) != ERROR_SUCCESS ||
        !owner) {
        if (sd) ::LocalFree(sd);
        return PipeOwner::Unreadable;
    }
    PipeOwner out = PipeOwner::Other;
    const QString text = sidText(owner);
    if (!text.isEmpty() && text == accountSid()) out = PipeOwner::Mine;
    if (out == PipeOwner::Other && (::IsWellKnownSid(owner, WinLocalSystemSid) ||
                                    ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid)))
        out = PipeOwner::Trusted;
    ::LocalFree(sd);
    return out;
}

/// Whether the pipe behind @p pipe is this install's server, read from the
/// pipe itself: its owner is this account, the system or an administrator, and
/// the program holding the other end, when it can be read, is this executable.
/// A pipe someone else signed in at this PC made under the same name fails the
/// first; nothing of what it would answer is read before this.
bool serverIsOurs(HANDLE pipe, QString* why)
{
    const PipeOwner owner = ownerOf(pipe);
    if (owner == PipeOwner::Unreadable) {
        *why = QStringLiteral("the channel's owner cannot be read");
        return false;
    }
    if (owner == PipeOwner::Other) {
        *why = QLatin1String(kOtherAccount);
        return false;
    }

    ULONG pid = 0;
    if (!::GetNamedPipeServerProcessId(pipe, &pid)) {
        *why = QStringLiteral("the program on the channel cannot be identified");
        return false;
    }
    // A server under the system account is not always ours to open: its owner,
    // checked above, then stands alone.
    const QString theirs = imageOf(pid);
    wchar_t ours[MAX_PATH * 2];
    const DWORD oursSize =
        ::GetModuleFileNameW(nullptr, ours, static_cast<DWORD>(sizeof(ours) / sizeof(ours[0])));
    if (theirs.isEmpty() || oursSize == 0) return true;
    const QString mine = QString::fromWCharArray(ours, static_cast<int>(oursSize));
    if (theirs.compare(mine, Qt::CaseInsensitive) != 0) {
        *why = QStringLiteral("the program on the channel is not this one: %1").arg(theirs);
        return false;
    }
    return true;
}

} // namespace

RequestGuard::LocalVerdict verdictOf(const LocalCaller& caller, bool service)
{
    return RequestGuard::localCallerVerdict(service, accountSid(), administratorsSid(), caller);
}

struct Server::Impl
{
    std::wstring path;
    QString endpoint;
    Handler handler;
    QObject* context = nullptr; // the Server, on its own thread

    PSECURITY_DESCRIPTOR sd = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE first = INVALID_HANDLE_VALUE; // the instance listen() made
    std::thread acceptor;
    bool listening = false;
    bool service = false; // opened to the people signed in at this PC (Audience::SignedIn)

    // Guards `context`: once `stopping`, no thread posts to it again.
    std::mutex dispatch;
    bool stopping = false;

    std::mutex idle;
    std::condition_variable idleCv;
    int active = 0;

    // Gone with the last holder: the server, or a caller still being served.
    ~Impl()
    {
        if (sd) ::LocalFree(sd);
        if (stopEvent) ::CloseHandle(stopEvent);
    }

    /// An instance of the pipe. Asked for as the first, it fails when the name
    /// exists already (ERROR_ACCESS_DENIED). A later one joins the instances
    /// this server holds, and is checked to sit on a pipe this account owns:
    /// had the name been free for an instant and taken, the instance would be
    /// on somebody else's pipe, under their access list. It is then refused
    /// the same way.
    HANDLE createInstance(bool firstInstance) const
    {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        const HANDLE pipe = ::CreateNamedPipeW(
            path.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (pipe != INVALID_HANDLE_VALUE && !firstInstance && ownerOf(pipe) != PipeOwner::Mine) {
            ::CloseHandle(pipe);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        return pipe;
    }

    /// Whether a caller is being served: its instance then still holds the name.
    bool serving()
    {
        std::lock_guard<std::mutex> lock(idle);
        return active > 0;
    }
};

namespace {

struct Pending
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    Reply reply;
};

/// Ends a caller's connection. After an answer, the caller is given a moment
/// to read it and hang up first: disconnecting at once would throw away what
/// it has not read yet, and waiting without a limit (FlushFileBuffers) would
/// let a caller that never reads keep this thread.
void finish(HANDLE pipe, bool answered)
{
    if (answered) {
        char byte = 0;
        DWORD got = 0;
        pipeIo(pipe, false, &byte, 1, &got, 2000);
    }
    ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);
}

} // namespace

static void serveConnection(const std::shared_ptr<Server::Impl>& impl, HANDLE pipe);

static void acceptLoop(const std::shared_ptr<Server::Impl>& impl)
{
    HANDLE pipe = impl->first;
    impl->first = INVALID_HANDLE_VALUE;
    int failures = 0;
    while (::WaitForSingleObject(impl->stopEvent, 0) != WAIT_OBJECT_0) {
        if (pipe == INVALID_HANDLE_VALUE) {
            // With no caller being served, no instance of ours is left and the
            // name is free: it is asked for as the first again, which fails if
            // something took it meanwhile. The channel then stays shut, and
            // says so, until the name is let go.
            pipe = impl->createInstance(!impl->serving());
            if (pipe == INVALID_HANDLE_VALUE) {
                const DWORD code = ::GetLastError();
                // The first time, then once a minute.
                if (failures++ % 60 == 0)
                    Logger::error(
                        code == ERROR_ACCESS_DENIED
                            ? QStringLiteral("[LocalChannel] the channel's name is held by another "
                                             "program: no channel until it lets go")
                            : QStringLiteral("[LocalChannel] the channel cannot be reopened (%1)")
                                  .arg(lastErrorText(code)));
                ::WaitForSingleObject(impl->stopEvent, 1000);
                continue;
            }
            if (failures > 0) Logger::info(QStringLiteral("[LocalChannel] open again"));
            failures = 0;
        }

        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ::ConnectNamedPipe(pipe, &ov) != 0;
        if (!connected) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {impl->stopEvent, ov.hEvent};
                const DWORD which = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (which == WAIT_OBJECT_0 + 1) {
                    DWORD ignored = 0;
                    connected = ::GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
                } else {
                    DWORD ignored = 0;
                    ::CancelIoEx(pipe, &ov);
                    ::GetOverlappedResult(pipe, &ov, &ignored, TRUE);
                }
            }
        }
        ::CloseHandle(ov.hEvent);

        if (!connected) {
            ::CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            continue;
        }

        // The next instance before this caller is served: the name is never
        // left without one of ours behind it.
        const HANDLE caller = pipe;
        pipe = impl->createInstance(false);

        bool room = false;
        {
            std::lock_guard<std::mutex> lock(impl->idle);
            if (impl->active < kMaxConnections) {
                ++impl->active;
                room = true;
            }
        }
        if (!room) {
            Logger::warning(QStringLiteral("[LocalChannel] a caller turned away: %1 are being "
                                           "served")
                                .arg(kMaxConnections));
            finish(caller, false);
            continue;
        }
        std::thread([impl, caller]() {
            serveConnection(impl, caller);
            std::lock_guard<std::mutex> lock(impl->idle);
            --impl->active;
            impl->idleCv.notify_all();
        }).detach();
    }
    if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
}

/// A thread that read a caller could not be given back its own identity: the
/// channel serves nobody else until the server starts again, and says so from
/// the server's thread. The rest of the server goes on. Never expected.
static void shutChannel(const std::shared_ptr<Server::Impl>& impl)
{
    {
        std::lock_guard<std::mutex> lock(impl->dispatch);
        if (!impl->stopping) {
            QMetaObject::invokeMethod(
                impl->context,
                []() {
                    Logger::error(QStringLiteral(
                        "[LocalChannel] a caller's identity could not be let go of on the "
                        "thread that read it: the channel is shut until the server starts "
                        "again"));
                },
                Qt::QueuedConnection);
            impl->stopping = true;
        }
    }
    ::SetEvent(impl->stopEvent);
}

static void serveConnection(const std::shared_ptr<Server::Impl>& impl, HANDLE pipe)
{
    FrameReader reader(true, kMaxRequestBytes);
    QString why;
    if (!readFrame(pipe, reader, kRequestReadMs, &why)) {
        Logger::warning(QStringLiteral("[LocalChannel] a caller refused: %1").arg(why));
        // Nothing is answered to what did not speak this protocol.
        finish(pipe, false);
        return;
    }
    // Who asks, from its own token, before anything it wrote is looked at.
    LocalCaller caller;
    if (readCaller(pipe, caller) == CallerRead::NotReverted) {
        shutChannel(impl);
        finish(pipe, false);
        return;
    }
    const std::optional<Request> request = decodeRequest(reader.payload());
    if (!request) {
        Logger::warning(QStringLiteral("[LocalChannel] a caller refused: not a request"));
        finish(pipe, writeAll(pipe, replyFrame(refusal(400, "bad_request")), kRequestReadMs));
        return;
    }

    auto pending = std::make_shared<Pending>();
    bool posted = false;
    {
        std::lock_guard<std::mutex> lock(impl->dispatch);
        if (!impl->stopping) {
            const Server::Handler handler = impl->handler;
            Request asked = *request;
            asked.caller = caller;
            asked.service = impl->service;
            asked.callerLabel = callerOf(pipe);
            posted = QMetaObject::invokeMethod(
                impl->context,
                [handler, asked, pending]() {
                    handler(asked, [pending](const Reply& reply) {
                        std::lock_guard<std::mutex> answer(pending->mutex);
                        if (pending->done) return; // answered once
                        pending->reply = reply;
                        pending->done = true;
                        pending->cv.notify_all();
                    });
                },
                Qt::QueuedConnection);
        }
    }

    Reply reply = refusal(503, "server_stopping");
    if (posted) {
        QElapsedTimer clock;
        clock.start();
        std::unique_lock<std::mutex> lock(pending->mutex);
        while (!pending->done && clock.elapsed() < kHandlerMs &&
               ::WaitForSingleObject(impl->stopEvent, 0) != WAIT_OBJECT_0)
            pending->cv.wait_for(lock, std::chrono::milliseconds(200));
        if (pending->done)
            reply = pending->reply;
        else if (clock.elapsed() >= kHandlerMs)
            reply = refusal(504, "no_answer_in_time");
    }
    finish(pipe, writeAll(pipe, replyFrame(reply), kRequestReadMs));
}

Server::Server(QObject* parent)
    : QObject(parent)
    , m_Impl(std::make_shared<Impl>())
{
    m_Impl->context = this;
}

Server::~Server()
{
    close();
}

bool Server::listen(const QString& endpoint, Handler handler, QString* error, Audience audience)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (m_Impl->listening) return fail(QStringLiteral("already listening"));
    if (!handler) return fail(QStringLiteral("no handler"));

    // A fresh state per listen(): a thread of an earlier one may still hold
    // the old.
    m_Impl = std::make_shared<Impl>();
    m_Impl->context = this;
    m_Impl->endpoint = endpoint;
    m_Impl->path = endpoint.toStdWString();
    m_Impl->handler = std::move(handler);
    m_Impl->service = audience == Audience::SignedIn;

    // Who may open it: pipeAccessList().
    const std::wstring self = ownSid();
    if (self.empty()) return fail(QStringLiteral("this account cannot be read"));
    const std::wstring sddl =
        pipeAccessList(QString::fromStdWString(self), audience).toStdWString();
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                &m_Impl->sd, nullptr))
        return fail(QStringLiteral("the channel's access list cannot be built (%1)")
                        .arg(lastErrorText(::GetLastError())));

    m_Impl->first = m_Impl->createInstance(true);
    if (m_Impl->first == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        ::LocalFree(m_Impl->sd);
        m_Impl->sd = nullptr;
        // ERROR_ACCESS_DENIED is what FILE_FLAG_FIRST_PIPE_INSTANCE answers
        // when the name exists already.
        return fail(
            code == ERROR_ACCESS_DENIED
                ? QStringLiteral("the channel's name is already held by another program")
                : QStringLiteral("the channel cannot be opened (%1)").arg(lastErrorText(code)));
    }
    m_Impl->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_Impl->stopEvent) {
        ::CloseHandle(m_Impl->first);
        m_Impl->first = INVALID_HANDLE_VALUE;
        ::LocalFree(m_Impl->sd);
        m_Impl->sd = nullptr;
        return fail(QStringLiteral("the channel cannot be opened (%1)")
                        .arg(lastErrorText(::GetLastError())));
    }
    m_Impl->listening = true;
    const std::shared_ptr<Impl> impl = m_Impl;
    m_Impl->acceptor = std::thread([impl]() { acceptLoop(impl); });
    return true;
}

void Server::close()
{
    if (!m_Impl->listening) return;
    {
        std::lock_guard<std::mutex> lock(m_Impl->dispatch);
        m_Impl->stopping = true;
    }
    ::SetEvent(m_Impl->stopEvent);
    if (m_Impl->acceptor.joinable()) m_Impl->acceptor.join();
    {
        // Callers being served finish on their own, each within its timeouts;
        // they hold the state, not this object.
        std::unique_lock<std::mutex> lock(m_Impl->idle);
        m_Impl->idleCv.wait_for(lock, std::chrono::seconds(3),
                                [this]() { return m_Impl->active == 0; });
    }
    m_Impl->listening = false;
    // The event and the access list stay with the state: a caller still being
    // served reads the first, and the process is leaving or listening anew.
}

bool Server::isListening() const
{
    return m_Impl->listening;
}

QString Server::endpoint() const
{
    return m_Impl->endpoint;
}

namespace {

Reply ask(const QString& endpoint, const Request& request, int timeoutMs, HANDLE* serverProcess,
          QString* error)
{
    if (serverProcess) *serverProcess = nullptr;
    auto fail = [error, serverProcess](const QString& why) {
        if (error) *error = why;
        if (serverProcess && *serverProcess) {
            ::CloseHandle(*serverProcess);
            *serverProcess = nullptr;
        }
        return Reply{};
    };
    const std::wstring path = endpoint.toStdWString();
    QElapsedTimer clock;
    clock.start();

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        // Read, and write data: all a caller is given, and all it needs. The
        // server may learn who asks and nothing more.
        pipe = ::CreateFileW(
            path.c_str(), GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) return fail(QStringLiteral("nothing listens"));
        if (code != ERROR_PIPE_BUSY)
            return fail(
                QStringLiteral("the channel cannot be opened (%1)").arg(lastErrorText(code)));
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0) return fail(QStringLiteral("timed out"));
        ::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(qMin<qint64>(left, 500)));
    }

    QString why;
    if (!serverIsOurs(pipe, &why)) {
        ::CloseHandle(pipe);
        return fail(why);
    }
    // The process to wait for, from the pipe and before the request: a PID
    // read once the server is gone could be another process's.
    if (serverProcess) {
        ULONG pid = 0;
        if (::GetNamedPipeServerProcessId(pipe, &pid))
            *serverProcess =
                ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!*serverProcess) {
            ::CloseHandle(pipe);
            return fail(QStringLiteral("the server cannot be waited for"));
        }
    }

    const QByteArray payload = encodeRequest(request);
    if (static_cast<quint32>(payload.size()) > kMaxRequestBytes) {
        ::CloseHandle(pipe);
        return fail(QStringLiteral("request too large"));
    }
    const QByteArray out = QByteArray(kPreamble) + frame(payload);
    const qint64 forWrite = timeoutMs - clock.elapsed();
    if (forWrite <= 0 || !writeAll(pipe, out, static_cast<int>(forWrite))) {
        ::CloseHandle(pipe);
        return fail(QStringLiteral("the request could not be sent"));
    }

    FrameReader reader(false, kMaxReplyBytes);
    const qint64 forRead = timeoutMs - clock.elapsed();
    if (forRead <= 0 || !readFrame(pipe, reader, static_cast<int>(forRead), &why)) {
        ::CloseHandle(pipe);
        return fail(forRead <= 0 ? QStringLiteral("timed out") : why);
    }
    ::CloseHandle(pipe);

    const std::optional<Reply> reply = decodeReply(reader.payload());
    if (!reply) return fail(QStringLiteral("the answer is not one of this channel's"));
    return *reply;
}

} // namespace

Reply request(const QString& endpoint, const Request& request, int timeoutMs, QString* error)
{
    return ask(endpoint, request, timeoutMs, nullptr, error);
}

Reply requestHoldingServer(const QString& endpoint, const Request& request, int timeoutMs,
                           void** serverProcess, QString* error)
{
    HANDLE process = nullptr;
    const Reply reply = ask(endpoint, request, timeoutMs, &process, error);
    if (serverProcess) {
        *serverProcess = process;
    } else if (process) {
        ::CloseHandle(process);
    }
    return reply;
}

#else // !Q_OS_WIN

// ═════════════════════════════════════════════════════════════════════════════
// Linux, macOS: a Unix socket under the data folder
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// The account at the other end of a connected socket, -1 when it cannot be
/// read. Asked of the kernel, never of what the peer wrote.
qint64 peerUid(qintptr fd)
{
#if defined(Q_OS_LINUX)
    struct ucred cred = {};
    socklen_t size = sizeof(cred);
    if (::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0) return -1;
    return static_cast<qint64>(cred.uid);
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(static_cast<int>(fd), &uid, &gid) != 0) return -1;
    return static_cast<qint64>(uid);
#endif
}

/// This account, or root: the two that may be at either end.
bool accountAllowed(qint64 uid)
{
    return uid >= 0 && (uid == static_cast<qint64>(::geteuid()) || uid == 0);
}

/// Who is at the other end, for the log.
QString callerOf(qintptr fd)
{
    QString pid;
#if defined(Q_OS_LINUX)
    struct ucred cred = {};
    socklen_t size = sizeof(cred);
    if (::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0)
        pid = QStringLiteral("pid %1, ").arg(cred.pid);
#endif
    return pid + QStringLiteral("account %1").arg(peerUid(fd));
}

/// The folder the socket lives in, made if need be and kept to this account
/// alone (0700). Refused when it is someone else's, a link, or open to others:
/// what is created inside must never be reachable before it is locked.
bool folderIsOursAlone(const QString& dir, QString* why)
{
    const QByteArray path = dir.toLocal8Bit();
    if (::mkdir(path.constData(), 0700) != 0 && errno != EEXIST) {
        *why = QStringLiteral("the channel's folder cannot be made");
        return false;
    }
    struct stat info = {};
    if (::lstat(path.constData(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != ::geteuid()) {
        *why = QStringLiteral("the channel's folder is not this account's");
        return false;
    }
    if ((info.st_mode & 077) != 0 && ::chmod(path.constData(), 0700) != 0) {
        *why = QStringLiteral("the channel's folder cannot be closed to other accounts");
        return false;
    }
    if (::lstat(path.constData(), &info) != 0 || (info.st_mode & 077) != 0) {
        *why = QStringLiteral("the channel's folder is open to other accounts");
        return false;
    }
    return true;
}

} // namespace

RequestGuard::LocalVerdict verdictOf(const LocalCaller& caller, bool service)
{
    // The kernel's word on the peer was taken when it connected: this
    // account or root, nobody else (accountAllowed).
    Q_UNUSED(caller);
    Q_UNUSED(service);
    return RequestGuard::LocalVerdict::Admit;
}

struct Server::Impl
{
    QLocalServer* server = nullptr;
    QString endpoint;
    Handler handler;
};

Server::Server(QObject* parent)
    : QObject(parent)
    , m_Impl(std::make_shared<Impl>())
{}

Server::~Server()
{
    close();
}

bool Server::listen(const QString& endpoint, Handler handler, QString* error, Audience audience)
{
    // The socket is this account's alone whoever the audience: see the header.
    Q_UNUSED(audience);
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (m_Impl->server) return fail(QStringLiteral("already listening"));
    if (!handler) return fail(QStringLiteral("no handler"));
    if (!QFileInfo(endpoint).isAbsolute())
        return fail(QStringLiteral("the channel needs a full path"));

    QString why;
    if (!folderIsOursAlone(QFileInfo(endpoint).absolutePath(), &why)) return fail(why);

    // A socket file left by a run that ended badly is removed, if it is this
    // account's; one that a program still answers on is somebody's, and is left
    // alone, as is anything there that is not a socket of ours.
    struct stat left = {};
    if (::lstat(endpoint.toLocal8Bit().constData(), &left) == 0) {
        if (!S_ISSOCK(left.st_mode) || left.st_uid != ::geteuid())
            return fail(QStringLiteral("the channel's name is held by something that is not "
                                       "this account's socket"));
        QLocalSocket probe;
        probe.connectToServer(endpoint);
        if (probe.waitForConnected(500))
            return fail(QStringLiteral("the channel's name is already held by another program"));
        QLocalServer::removeServer(endpoint);
    }

    auto* server = new QLocalServer(this);
    // The server's account alone (0600).
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(endpoint)) {
        const QString why = server->errorString();
        delete server;
        return fail(QStringLiteral("the channel cannot be opened (%1)").arg(why));
    }
    m_Impl->server = server;
    m_Impl->endpoint = endpoint;
    m_Impl->handler = std::move(handler);

    connect(server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket* socket = m_Impl->server->nextPendingConnection()) {
            const qint64 uid = peerUid(socket->socketDescriptor());
            if (!accountAllowed(uid)) {
                const QString line = refusals().note(uid < 0 ? RefusalLog::Kind::Unreadable
                                                             : RefusalLog::Kind::OtherAccount);
                if (!line.isEmpty()) Logger::info(line);
                socket->abort();
                socket->deleteLater();
                continue;
            }
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);

            auto reader = std::make_shared<FrameReader>(true, kMaxRequestBytes);
            auto asked = std::make_shared<bool>(false);
            // A caller that connects and says nothing does not keep its place.
            QTimer::singleShot(kRequestReadMs, socket, [socket, asked]() {
                if (!*asked) socket->abort();
            });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, reader, asked]() {
                const QByteArray bytes = socket->readAll();
                if (*asked) {
                    // One request, one answer.
                    socket->abort();
                    return;
                }
                const FrameReader::State state = reader->feed(bytes);
                if (state == FrameReader::State::NeedMore) return;
                if (state == FrameReader::State::Bad) {
                    Logger::warning(
                        QStringLiteral("[LocalChannel] a caller refused: %1").arg(reader->error()));
                    socket->abort();
                    return;
                }
                *asked = true;
                const QPointer<QLocalSocket> to(socket);
                auto answered = std::make_shared<bool>(false);
                auto respond = [to, answered](const Reply& reply) {
                    if (*answered || !to) return;
                    *answered = true;
                    to->write(replyFrame(reply));
                    to->flush();
                    to->disconnectFromServer();
                };
                const std::optional<Request> request = decodeRequest(reader->payload());
                if (!request) {
                    Logger::warning(
                        QStringLiteral("[LocalChannel] a caller refused: not a request"));
                    respond(refusal(400, "bad_request"));
                    return;
                }
                // Its account was judged when it connected (accountAllowed).
                Request asked = *request;
                asked.callerLabel = callerOf(socket->socketDescriptor());
                QTimer::singleShot(kHandlerMs, socket,
                                   [respond]() { respond(refusal(504, "no_answer_in_time")); });
                m_Impl->handler(asked, respond);
            });
        }
    });
    return true;
}

void Server::close()
{
    if (!m_Impl->server) return;
    m_Impl->server->close();
    delete m_Impl->server;
    m_Impl->server = nullptr;
}

bool Server::isListening() const
{
    return m_Impl->server != nullptr;
}

QString Server::endpoint() const
{
    return m_Impl->endpoint;
}

Reply request(const QString& endpoint, const Request& request, int timeoutMs, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error) *error = why;
        return Reply{};
    };
    // The socket file and the folder it is in must be this account's, or
    // root's, and the folder closed to everyone else's writes, before anything
    // is sent to whatever listens there.
    struct stat info = {};
    if (::lstat(endpoint.toLocal8Bit().constData(), &info) != 0)
        return fail(QStringLiteral("nothing listens"));
    if (!S_ISSOCK(info.st_mode) || !accountAllowed(static_cast<qint64>(info.st_uid)))
        return fail(QLatin1String(kOtherAccount));
    struct stat folder = {};
    if (::lstat(QFileInfo(endpoint).absolutePath().toLocal8Bit().constData(), &folder) != 0 ||
        !S_ISDIR(folder.st_mode) || !accountAllowed(static_cast<qint64>(folder.st_uid)) ||
        (folder.st_mode & 022) != 0)
        return fail(QStringLiteral("the channel's folder belongs to another account"));

    QElapsedTimer clock;
    clock.start();
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(timeoutMs)) return fail(QStringLiteral("nothing listens"));
    if (!accountAllowed(peerUid(socket.socketDescriptor()))) {
        socket.abort();
        return fail(QLatin1String(kOtherAccount));
    }

    const QByteArray payload = encodeRequest(request);
    if (static_cast<quint32>(payload.size()) > kMaxRequestBytes)
        return fail(QStringLiteral("request too large"));
    socket.write(QByteArray(kPreamble) + frame(payload));
    while (socket.bytesToWrite() > 0) {
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0 || !socket.waitForBytesWritten(static_cast<int>(left)))
            return fail(QStringLiteral("the request could not be sent"));
    }

    FrameReader reader(false, kMaxReplyBytes);
    reader.feed(socket.readAll());
    while (reader.state() == FrameReader::State::NeedMore) {
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0) return fail(QStringLiteral("timed out"));
        if (!socket.waitForReadyRead(static_cast<int>(left))) {
            reader.feed(socket.readAll());
            if (reader.state() != FrameReader::State::NeedMore) break;
            return fail(socket.state() == QLocalSocket::ConnectedState
                            ? QStringLiteral("timed out")
                            : QStringLiteral("the other end closed or went silent"));
        }
        reader.feed(socket.readAll());
    }
    if (reader.state() == FrameReader::State::Bad) return fail(reader.error());

    const std::optional<Reply> reply = decodeReply(reader.payload());
    if (!reply) return fail(QStringLiteral("the answer is not one of this channel's"));
    return *reply;
}

#endif // Q_OS_WIN

} // namespace mw::localchannel
