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

#include "WorkerService.h"

#include "ConsoleSession.h"
#include "RemoteDesktopReturnPolicy.h"
#include "common/Edition.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>
#endif

namespace {

#ifdef Q_OS_WIN

/// Once a launch through the service has failed, every later one in this run
/// would fail the same way: stop offering it, and let the elevated task — or a
/// plain child — carry the session instead.
std::atomic<bool> g_ServiceBroken{false};

/// How long a caller waits for the service to answer, and how long the service
/// waits for a caller that has connected to say what it wants. Both are local
/// round trips measured in milliseconds; the seconds here are the margin for a
/// machine under load, not an expected duration.
constexpr DWORD kRequestTimeoutMs = 5000;

/// The longest request the control pipe will read. A name is at most 128
/// characters (validBase); the rest is margin for the newline and for saying
/// "too long" rather than hanging on a client that never stops writing.
constexpr DWORD kMaxRequestBytes = 512;

std::wstring controlPipePath()
{
    return (QStringLiteral("\\\\.\\pipe\\") + WorkerService::controlPipeName()).toStdWString();
}

/// The one field a request carries, and the whole of what it may be: a pipe
/// base name the server invented. No separator, no dot-dot, no UNC, nothing
/// that could grow a path — it is only ever concatenated after `\\.\pipe\`.
bool validBase(const QString& base)
{
    static const QRegularExpression allowed(QStringLiteral("\\A[A-Za-z0-9._-]{1,128}\\z"));
    return allowed.match(base).hasMatch();
}

QString ownExecutable()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

/// The service's image path, as the SCM holds it: the quoted executable, with
/// the arguments that follow stripped. Empty when the service is not there.
QString registeredImage(QString* error = nullptr)
{
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QString();
    };
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return fail(
            QStringLiteral("cannot reach the service manager (error %1)").arg(::GetLastError()));
    const std::wstring name = WorkerService::serviceName().toStdWString();
    SC_HANDLE service = ::OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG);
    if (!service) {
        const DWORD err = ::GetLastError();
        ::CloseServiceHandle(scm);
        return fail(err == ERROR_SERVICE_DOES_NOT_EXIST
                        ? QStringLiteral("the service is not installed")
                        : QStringLiteral("cannot open the service (error %1)").arg(err));
    }
    DWORD needed = 0;
    ::QueryServiceConfigW(service, nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed);
    QString image;
    if (needed &&
        ::QueryServiceConfigW(service, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data()),
                              needed, &needed)) {
        const auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
        image = QString::fromWCharArray(config->lpBinaryPathName).trimmed();
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(scm);
    if (image.isEmpty()) return fail(QStringLiteral("the service has no image path"));
    // `"C:\...\MoonlightWeb.exe" --worker-service` — the quoted head is the image.
    if (image.startsWith(QLatin1Char('"'))) {
        const int end = image.indexOf(QLatin1Char('"'), 1);
        image = end > 0 ? image.mid(1, end - 1) : image.mid(1);
    } else {
        const int space = image.indexOf(QLatin1Char(' '));
        if (space > 0) image = image.left(space);
    }
    return image;
}

/// The console user's roaming AppData, so a SYSTEM worker leaves its log and
/// its minidumps where the person who installed this will look for them —
/// beside the server's — rather than under systemprofile. Empty when it cannot
/// be had, which only costs the worker its diagnostics.
QString userAppData(HANDLE userToken)
{
    PWSTR path = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, userToken,
                                      &path)) ||
        !path)
        return {};
    const QString roaming = QString::fromWCharArray(path);
    ::CoTaskMemFree(path);
    // Mirrors QStandardPaths::AppDataLocation on Windows: <Roaming>/<org>/<app>.
    return roaming + QLatin1Char('/') + QCoreApplication::organizationName() + QLatin1Char('/') +
           mw::edition::dataName();
}

// ── The service side ────────────────────────────────────────────────────────

SERVICE_STATUS_HANDLE g_StatusHandle = nullptr;
SERVICE_STATUS g_Status = {};
HANDLE g_StopEvent = nullptr;

void report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0)
{
    static DWORD checkPoint = 1;
    g_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState = state;
    g_Status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP;
    g_Status.dwWin32ExitCode = exitCode;
    g_Status.dwWaitHint = waitHintMs;
    g_Status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    if (g_StatusHandle) ::SetServiceStatus(g_StatusHandle, &g_Status);
}

DWORD WINAPI serviceControl(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING, NO_ERROR, 2000);
        if (g_StopEvent) ::SetEvent(g_StopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE: report(g_Status.dwCurrentState); return NO_ERROR;
    default: return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/// One overlapped read or write with a deadline, on the control pipe.
bool pipeIo(HANDLE pipe, bool write, void* data, DWORD size, DWORD* done, DWORD timeoutMs)
{
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
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

/// Who asked: this same executable or not, and from which session as which
/// user. Read from the pipe itself, never from what the caller wrote.
bool identifyCaller(HANDLE pipe, QString* why, DWORD* session, std::wstring* callerSid)
{
    ULONG pid = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &pid)) {
        *why = QStringLiteral("cannot identify the caller (error %1)").arg(::GetLastError());
        return false;
    }
    const QString image = ConsoleSession::processImage(pid);
    if (!ConsoleSession::isThisExecutable(image)) {
        *why = QStringLiteral("caller is \"%1\" (pid %2), not this executable").arg(image).arg(pid);
        return false;
    }

    // Identification level is all the client grants and all that is needed:
    // the token is read, never acted with.
    if (!::ImpersonateNamedPipeClient(pipe)) {
        *why = QStringLiteral("cannot read the caller's token (error %1)").arg(::GetLastError());
        return false;
    }
    HANDLE token = nullptr;
    *session = 0xFFFFFFFF;
    callerSid->clear();
    if (::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        DWORD size = 0;
        if (!::GetTokenInformation(token, TokenSessionId, session, sizeof(*session), &size))
            *session = 0xFFFFFFFF;
        size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size);
        LPWSTR sid = nullptr;
        if (size && ::GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
            ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
            *callerSid = sid;
            ::LocalFree(sid);
        }
        ::CloseHandle(token);
    }
    ::RevertToSelf();

    if (*session == 0xFFFFFFFF || callerSid->empty()) {
        *why = QStringLiteral("the caller's session or user could not be read");
        return false;
    }
    return true;
}

/// Who asked, and whether they may. Two questions, both about the caller and
/// neither about what they sent: are they this same executable, and are they on
/// the console session this service would put a SYSTEM process into? The
/// caller's user SID comes back in @p callerSid: the worker is made theirs to
/// look at (launchWorker).
bool callerAllowed(HANDLE pipe, QString* why, std::wstring* callerSid)
{
    DWORD session = 0xFFFFFFFF;
    if (!identifyCaller(pipe, why, &session, callerSid)) return false;
    const DWORD console = ::WTSGetActiveConsoleSessionId();
    if (session != console) {
        *why = QStringLiteral("the caller is on session %1, the console is %2")
                   .arg(session)
                   .arg(console);
        return false;
    }
    return true;
}

/// How launchWorker() starts one worker (its defaults: the ordinary one).
struct LaunchSpec
{
    /// The session the worker starts in. 0xFFFFFFFF: the console's, read now.
    /// The sign-in screen's is the one its checks were made on, never read a
    /// second time.
    DWORD session = 0xFFFFFFFF;
    /// `--signin-screen`: the worker streams the sign-in screen and nothing
    /// else (platform/SignInScreen.h). Set here, on the command line this
    /// service writes; nothing a caller sends can set or clear it.
    bool signInScreen = false;
    /// Whose AppData gets the worker's log when the worker's own session has
    /// nobody signed in: the caller's session. 0xFFFFFFFF: none.
    DWORD logSession = 0xFFFFFFFF;
    /// Where to keep the worker's process handle (SYNCHRONIZE at least)
    /// instead of closing it.
    HANDLE* process = nullptr;
};

/// Start the worker: this executable, as SYSTEM, on the console session's
/// desktop, attached to the pipes named after `base`.
///
/// The SYSTEM token is this service's own, duplicated and moved to the console
/// session — SetTokenInformation(TokenSessionId) needs SeTcbPrivilege, which
/// LocalSystem holds. `winsta0\default` is named explicitly: a service's own
/// default is a window station nobody sees, and a worker started there would
/// capture nothing and inject nowhere.
///
/// The process is given a DACL of its own. Left to the SYSTEM token's default,
/// only SYSTEM and administrators could open it, and the user's server — not
/// elevated, the way it always runs — could not read the image it checks
/// before trusting the pipes: it saw "", refused the worker and fell back to
/// the elevated task, which cannot reach the secure desktop. Every session of
/// v0.3.1 did (bench 28/09/2026). The user who asked (@p callerSid) gets what
/// they have on their elevated worker: its image and exit code
/// (PROCESS_QUERY_LIMITED_INFORMATION), a wait on it (SYNCHRONIZE) and
/// stopping it (PROCESS_TERMINATE). Nothing that reads or writes its memory,
/// its threads or its token.
bool launchWorker(const QString& base, const std::wstring& callerSid, DWORD* pid, QString* error,
                  const LaunchSpec& spec = {})
{
    auto fail = [&](const QString& why) {
        *error = why;
        return false;
    };
    DWORD console = spec.session != 0xFFFFFFFF ? spec.session : ::WTSGetActiveConsoleSessionId();
    if (console == 0xFFFFFFFF) return fail(QStringLiteral("there is no console session"));

    HANDLE self = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self))
        return fail(QStringLiteral("OpenProcessToken failed (error %1)").arg(::GetLastError()));
    HANDLE token = nullptr;
    const bool duplicated = ::DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr,
                                               SecurityImpersonation, TokenPrimary, &token) != 0;
    ::CloseHandle(self);
    if (!duplicated)
        return fail(QStringLiteral("DuplicateTokenEx failed (error %1)").arg(::GetLastError()));
    if (!::SetTokenInformation(token, TokenSessionId, &console, sizeof(console))) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(token);
        return fail(
            QStringLiteral("cannot move the token to session %1 (error %2)").arg(console).arg(err));
    }

    // The worker's diagnostics belong with the user's, not under systemprofile.
    // Built from the console user's token when there is one; a machine sitting
    // at the login screen has none, and the worker then logs where SYSTEM does.
    QString dataDir;
    HANDLE userToken = nullptr;
    if (::WTSQueryUserToken(console, &userToken)) {
        dataDir = userAppData(userToken);
        ::CloseHandle(userToken);
    } else if (spec.logSession != 0xFFFFFFFF && ::WTSQueryUserToken(spec.logSession, &userToken)) {
        // The sign-in screen's worker: beside the server that asked for it.
        dataDir = userAppData(userToken);
        ::CloseHandle(userToken);
    }

    QString commandLine = QLatin1Char('"') + ownExecutable() +
                          QStringLiteral("\" --stream-worker --worker-pipe ") + base;
    if (mw::edition::devFlag()) commandLine += QStringLiteral(" --dev");
    if (spec.signInScreen) commandLine += QStringLiteral(" --signin-screen");
    if (!dataDir.isEmpty())
        commandLine += QStringLiteral(" --worker-data-dir \"") + QDir::toNativeSeparators(dataDir) +
                       QLatin1Char('"');

    LPVOID env = nullptr;
    if (!::CreateEnvironmentBlock(&env, token, FALSE)) env = nullptr;

    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = desktop;

    std::wstring cmd = commandLine.toStdWString();
    const std::wstring app = ownExecutable().toStdWString();
    const std::wstring cwd = QFileInfo(ownExecutable()).absolutePath().toStdWString();

    // SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE.
    const std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GRGX;;;BA)(A;;0x101001;;;" + callerSid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                                nullptr)) {
        const DWORD sddlError = ::GetLastError();
        if (env) ::DestroyEnvironmentBlock(env);
        ::CloseHandle(token);
        return fail(QStringLiteral("cannot build the worker's DACL (error %1)").arg(sddlError));
    }
    SECURITY_ATTRIBUTES processSecurity = {};
    processSecurity.nLength = sizeof(processSecurity);
    processSecurity.lpSecurityDescriptor = sd;

    PROCESS_INFORMATION pi = {};
    const BOOL created = ::CreateProcessAsUserW(
        token, app.c_str(), cmd.data(), &processSecurity, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, cwd.c_str(), &si, &pi);
    const DWORD err = ::GetLastError();
    ::LocalFree(sd);
    if (env) ::DestroyEnvironmentBlock(env);
    ::CloseHandle(token);
    if (!created) return fail(QStringLiteral("CreateProcessAsUser failed (error %1)").arg(err));

    *pid = pi.dwProcessId;
    ::CloseHandle(pi.hThread);
    if (spec.process)
        *spec.process = pi.hProcess;
    else
        ::CloseHandle(pi.hProcess);
    return true;
}

/// The request that asks for Ctrl+Alt+Suppr rather than for a worker. Not a
/// pipe base name — '!' is outside what validBase accepts — so no request can
/// be read as both.
constexpr char kSecureAttentionRequest[] = "sas!";

/// Ctrl+Alt+Suppr on the console session, pressed from here. Windows honours
/// SendSAS from a service in session 0 and ignores it, without a word, from a
/// SYSTEM process in the console session — which is what the worker is. Seen
/// on 29/09/2026: the worker's own call opened nothing; the same call from a
/// LocalSystem service opened the screen within a second. The machine's
/// policy has to let services press it (SoftwareSASGeneration 1 or 3, which
/// the installer sets); read first, so that a refusal is a reason in the log
/// rather than a screen that never opens.
bool pressSecureAttention(QString* error)
{
    DWORD policy = 0;
    DWORD size = sizeof(policy);
    const LSTATUS read = ::RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"SoftwareSASGeneration", RRF_RT_REG_DWORD, nullptr, &policy, &size);
    if (read != ERROR_SUCCESS) {
        *error = QStringLiteral("the machine's SoftwareSASGeneration policy is not set: no "
                                "service may press it");
        return false;
    }
    if (policy != 1 && policy != 3) {
        *error = QStringLiteral("the machine's SoftwareSASGeneration policy (%1) lets no service "
                                "press it")
                     .arg(policy);
        return false;
    }
    // From System32 only: this is a LocalSystem process loading a library.
    using SendSasFn = VOID(WINAPI*)(BOOL);
    static const HMODULE library =
        ::LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const SendSasFn sendSas = library ? reinterpret_cast<SendSasFn>(reinterpret_cast<void*>(
                                                   ::GetProcAddress(library, "SendSAS")))
                                             : nullptr;
    if (!sendSas) {
        *error = QStringLiteral("sas.dll has no SendSAS on this machine");
        return false;
    }
    // FALSE: as the service this is, not as the interactive user.
    sendSas(FALSE);
    return true;
}

void answerCaller(HANDLE pipe, const QString& answer)
{
    const QByteArray line = answer.toUtf8() + '\n';
    DWORD written = 0;
    pipeIo(pipe, true, const_cast<char*>(line.constData()), static_cast<DWORD>(line.size()),
           &written, kRequestTimeoutMs);
    ::FlushFileBuffers(pipe);
}

/// The request that asks for the caller's own session to be put back on the
/// console. Outside validBase too ('!'), like kSecureAttentionRequest.
constexpr char kConsoleReturnRequest[] = "console!";

/// The state of session @p id (RemoteDesktopReturnPolicy::SessionState), -1
/// when Windows will not say.
int sessionState(DWORD id)
{
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSConnectState, &buffer,
                                       &bytes) ||
        !buffer)
        return -1;
    const int value = bytes >= sizeof(WTS_CONNECTSTATE_CLASS)
                          ? static_cast<int>(*reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(buffer))
                          : -1;
    ::WTSFreeMemory(buffer);
    return value;
}

/// Somebody is signed in on session @p id. A session at the sign-in screen has
/// no user token and no user name. "Yes" is also the answer whenever Windows
/// will not tell: a session is never moved over a console that might be
/// somebody's.
bool userSignedIn(DWORD id)
{
    HANDLE token = nullptr;
    if (::WTSQueryUserToken(id, &token)) {
        ::CloseHandle(token);
        return true;
    }
    if (::GetLastError() != ERROR_NO_TOKEN) return true;
    LPWSTR name = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSUserName, &name, &bytes))
        return true;
    const bool someone = name && name[0] != 0;
    ::WTSFreeMemory(name);
    return someone;
}

/// The caller's session was left by Remote Desktop: put it back on the console,
/// as `tscon <id> /dest:console` does — SYSTEM may, without the user's
/// password. Only the caller's own session, only while Disconnected, and never
/// over a console somebody is signed in on (RemoteDesktopReturnPolicy). The
/// caller locks the session as soon as it is there; this side cannot, since
/// LockWorkStation works from the session's own desktop only.
void serveConsoleReturn(HANDLE pipe)
{
    QString why;
    DWORD session = 0xFFFFFFFF;
    std::wstring callerSid;
    RemoteDesktopReturnPolicy::ConsoleReturnFacts facts;
    if (identifyCaller(pipe, &why, &session, &callerSid)) {
        facts.callerIsThisExecutable = true;
        facts.callerSession = session;
        facts.callerState = sessionState(session);
    }
    facts.consoleSession = ::WTSGetActiveConsoleSessionId();
    facts.consoleUserPresent =
        facts.consoleSession == 0xFFFFFFFF || userSignedIn(facts.consoleSession);
    if (why.isEmpty()) why = RemoteDesktopReturnPolicy::consoleReturnRefusal(facts);
    if (!why.isEmpty()) {
        Logger::warning("[WorkerService] refused to return a session to the console: " + why);
        answerCaller(pipe, QStringLiteral("err ") + why);
        return;
    }
    wchar_t noPassword[] = L"";
    if (!::WTSConnectSessionW(session, facts.consoleSession, noPassword, FALSE)) {
        const DWORD err = ::GetLastError();
        why = QStringLiteral("WTSConnectSession(%1 -> %2) failed (error %3)")
                  .arg(session)
                  .arg(facts.consoleSession)
                  .arg(err);
        Logger::warning("[WorkerService] " + why);
        answerCaller(pipe, QStringLiteral("err ") + why);
        return;
    }
    Logger::info(QStringLiteral("[WorkerService] session %1, left by Remote Desktop, is back on "
                                "the console (in place of session %2)")
                     .arg(session)
                     .arg(facts.consoleSession));
    answerCaller(pipe, QStringLiteral("ok"));
}

/// The request that sends the caller's own session away from the screen
/// again. Outside validBase too ('!').
constexpr char kHideRequest[] = "hide!";

/// A return that could not lock the session: rather than leave the desktop
/// unlocked on the PC's screen, it goes back to where Remote Desktop had left
/// it. The server tries that itself first (a user may disconnect their own
/// session); this is for when Windows refuses it there. Only the caller's own
/// session, only while it is Active on the console (RemoteDesktopReturnPolicy).
void serveHide(HANDLE pipe)
{
    QString why;
    DWORD session = 0xFFFFFFFF;
    std::wstring callerSid;
    RemoteDesktopReturnPolicy::ConsoleReturnFacts facts;
    if (identifyCaller(pipe, &why, &session, &callerSid)) {
        facts.callerIsThisExecutable = true;
        facts.callerSession = session;
        facts.callerState = sessionState(session);
    }
    facts.consoleSession = ::WTSGetActiveConsoleSessionId();
    if (why.isEmpty()) why = RemoteDesktopReturnPolicy::hideRefusal(facts);
    if (why.isEmpty() && !::WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, session, FALSE))
        why = QStringLiteral("WTSDisconnectSession(%1) failed (error %2)")
                  .arg(session)
                  .arg(::GetLastError());
    if (!why.isEmpty()) {
        Logger::warning("[WorkerService] did not send a session away from the console: " + why);
        answerCaller(pipe, QStringLiteral("err ") + why);
        return;
    }
    Logger::warning(QStringLiteral("[WorkerService] session %1 could not be locked on the console "
                                   "and was disconnected again")
                        .arg(session));
    answerCaller(pipe, QStringLiteral("ok"));
}

/// The request for a worker on the console's sign-in screen, followed by the
/// pipe base name: "signin!<base>". Outside validBase too ('!').
constexpr char kSignInRequestPrefix[] = "signin!";

/// The sign-in screen's worker while it runs: one at a time. Its process
/// handle (this service created it) and its id. The control loop's thread
/// alone touches them, one request at a time.
HANDLE g_SignInWorker = nullptr;
DWORD g_SignInWorkerPid = 0;

/// Whether the sign-in screen's worker still runs; forgets it once it has
/// ended.
bool signInWorkerAlive()
{
    if (!g_SignInWorker) return false;
    if (::WaitForSingleObject(g_SignInWorker, 0) == WAIT_TIMEOUT) return true;
    ::CloseHandle(g_SignInWorker);
    g_SignInWorker = nullptr;
    g_SignInWorkerPid = 0;
    return false;
}

/// Remote Desktop left the caller's session Disconnected, with no display at
/// all, and the console shows the sign-in screen of a session nobody is
/// signed in on: start a worker there, in sign-in screen mode, so that the
/// user signs in through the stream with their own credentials and Windows
/// itself brings their session back. The exception to callerAllowed()'s "the
/// caller is on the console", and only under RemoteDesktopReturnPolicy's
/// rule: the version offers it, the caller is this executable in its own
/// Disconnected session, the console is another session nobody is signed in
/// on, and no other sign-in screen worker runs. The worker goes into exactly
/// the console session those checks were made on, and its mode is on the
/// command line this service writes.
void serveSignIn(HANDLE pipe, const QString& base)
{
    QString why;
    DWORD session = 0xFFFFFFFF;
    std::wstring callerSid;
    RemoteDesktopReturnPolicy::SignInFacts facts;
    if (!validBase(base)) {
        why = QStringLiteral("the pipe name is not one this service accepts");
    } else if (identifyCaller(pipe, &why, &session, &callerSid)) {
        facts.callerIsThisExecutable = true;
        facts.callerSession = session;
        facts.callerState = sessionState(session);
    }
    facts.consoleSession = ::WTSGetActiveConsoleSessionId();
    facts.consoleUserPresent =
        facts.consoleSession == 0xFFFFFFFF || userSignedIn(facts.consoleSession);
    facts.signInWorkerAlive = signInWorkerAlive();
    if (why.isEmpty()) why = RemoteDesktopReturnPolicy::signInLaunchRefusal(facts);
    if (!why.isEmpty()) {
        Logger::warning("[WorkerService] refused a worker on the sign-in screen: " + why);
        answerCaller(pipe, QStringLiteral("err ") + why);
        return;
    }

    LaunchSpec spec;
    spec.session = facts.consoleSession;
    spec.signInScreen = true;
    spec.logSession = session;
    HANDLE process = nullptr;
    spec.process = &process;
    DWORD pid = 0;
    if (!launchWorker(base, callerSid, &pid, &why, spec)) {
        Logger::warning("[WorkerService] could not start the sign-in screen's worker: " + why);
        answerCaller(pipe, QStringLiteral("err ") + why);
        return;
    }
    g_SignInWorker = process;
    g_SignInWorkerPid = pid;
    Logger::info(QStringLiteral("[WorkerService] sign-in screen worker started on console session "
                                "%1, for session %2 that Remote Desktop left, pid=%3")
                     .arg(facts.consoleSession)
                     .arg(session)
                     .arg(pid));
    answerCaller(pipe, QStringLiteral("ok %1").arg(pid));
}

/// A worker asks for Ctrl+Alt+Suppr: the same two questions about who asks as
/// for a worker, then the key press. On a console nobody is signed in on, the
/// only worker there is the sign-in screen's: it alone may ask, and only while
/// it runs.
void serveSecureAttention(HANDLE pipe)
{
    QString answer;
    QString why;
    std::wstring callerSid;
    ULONG callerPid = 0;
    const DWORD console = ::WTSGetActiveConsoleSessionId();
    const bool emptyConsole = console != 0xFFFFFFFF && !userSignedIn(console);
    if (!callerAllowed(pipe, &why, &callerSid)) {
        answer = QStringLiteral("err refused");
        Logger::warning("[WorkerService] refused Ctrl+Alt+Suppr: " + why);
    } else if (emptyConsole &&
               (!signInWorkerAlive() || !::GetNamedPipeClientProcessId(pipe, &callerPid) ||
                callerPid != g_SignInWorkerPid)) {
        answer = QStringLiteral("err refused");
        Logger::warning("[WorkerService] refused Ctrl+Alt+Suppr: nobody is signed in on the "
                        "console, and the caller is not its sign-in screen's worker");
    } else if (!pressSecureAttention(&why)) {
        answer = QStringLiteral("err ") + why;
        Logger::warning("[WorkerService] Ctrl+Alt+Suppr not pressed: " + why);
    } else {
        answer = QStringLiteral("ok");
        Logger::info("[WorkerService] Ctrl+Alt+Suppr pressed on the console session");
    }
    answerCaller(pipe, answer);
}

void serveOne(HANDLE pipe)
{
    char buffer[kMaxRequestBytes];
    DWORD got = 0;
    if (!pipeIo(pipe, false, buffer, sizeof(buffer) - 1, &got, kRequestTimeoutMs) || got == 0) {
        Logger::warning("[WorkerService] a caller connected and said nothing");
        return;
    }
    buffer[got] = '\0';
    QString base = QString::fromLatin1(buffer, static_cast<int>(got)).trimmed();
    if (base == QLatin1String(kSecureAttentionRequest)) {
        serveSecureAttention(pipe);
        return;
    }
    if (base == QLatin1String(kConsoleReturnRequest)) {
        serveConsoleReturn(pipe);
        return;
    }
    if (base == QLatin1String(kHideRequest)) {
        serveHide(pipe);
        return;
    }
    if (base.startsWith(QLatin1String(kSignInRequestPrefix))) {
        serveSignIn(pipe, base.mid(static_cast<int>(sizeof(kSignInRequestPrefix) - 1)));
        return;
    }

    QString answer;
    QString why;
    std::wstring callerSid;
    DWORD pid = 0;
    if (!validBase(base)) {
        answer = QStringLiteral("err the pipe name is not one this service accepts");
        Logger::warning("[WorkerService] refused a malformed pipe name");
    } else if (!callerAllowed(pipe, &why, &callerSid)) {
        answer = QStringLiteral("err refused");
        Logger::warning("[WorkerService] refused a request: " + why);
    } else if (!userSignedIn(::WTSGetActiveConsoleSessionId())) {
        // A caller on a console nobody is signed in on is the sign-in
        // screen's own worker: a worker without its mode is never its to get.
        answer = QStringLiteral("err refused");
        Logger::warning("[WorkerService] refused a request: nobody is signed in on the console "
                        "(only a sign-in screen worker runs there)");
    } else if (!launchWorker(base, callerSid, &pid, &why)) {
        answer = QStringLiteral("err ") + why;
        Logger::warning("[WorkerService] could not start the worker: " + why);
    } else {
        answer = QStringLiteral("ok %1").arg(pid);
        Logger::info(QStringLiteral("[WorkerService] SYSTEM worker started, pid=%1").arg(pid));
    }
    answerCaller(pipe, answer);
}

void controlLoop()
{
    // SYSTEM and Administrators in full; the interactive user may open, write a
    // request and read the answer, and nothing else. The default DACL of a
    // named pipe would let every logon session on the machine in, including a
    // service account and a second, remote, desktop.
    // The user may not make an instance of the pipe (kControlPipeSddl).
    PSECURITY_DESCRIPTOR sd = nullptr;
    const std::wstring sddl = QString::fromLatin1(WorkerService::kControlPipeSddl).toStdWString();
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                                nullptr)) {
        Logger::error(QStringLiteral("[WorkerService] cannot build the control pipe's DACL "
                                     "(error %1)")
                          .arg(::GetLastError()));
        return;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;

    const std::wstring path = controlPipePath();
    // While the service runs, an instance of its pipe always waits for the
    // next caller: the next one is made before the one just served is closed,
    // so the name is never free for anything else to take. The first one,
    // and any made after the name was let go, asks to be the first instance
    // of that name: when something else holds it already, the service says
    // so loudly (callers refuse to talk to it anyway, askService).
    const auto makeInstance = [&](bool first) {
        return ::CreateNamedPipeW(
            path.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kMaxRequestBytes, kMaxRequestBytes, 0, &sa);
    };
    // A refusal is said when it starts, again only if its reason changes,
    // and once when it ends with how many tries it took: a name held for days
    // must not write a line a second.
    int refusedTries = 0;
    bool refusedHeld = false;
    DWORD refusedError = 0;
    const auto reportRefused = [&](bool first, DWORD err) {
        // ERROR_ACCESS_DENIED is what FILE_FLAG_FIRST_PIPE_INSTANCE answers
        // when the name exists already.
        const bool held = first && err == ERROR_ACCESS_DENIED;
        const bool news = refusedTries == 0 || held != refusedHeld;
        ++refusedTries;
        refusedHeld = held;
        refusedError = err;
        if (!news) return;
        if (held)
            Logger::error(QStringLiteral("[WorkerService] ANOTHER PROGRAM HOLDS this service's "
                                         "control pipe name: no worker can be asked for until "
                                         "it lets go (tried again every second, said once)"));
        else
            Logger::error(QStringLiteral("[WorkerService] CreateNamedPipe failed (error %1), "
                                         "tried again every second, said once")
                              .arg(err));
    };
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool first = true;
    while (::WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        if (pipe == INVALID_HANDLE_VALUE) {
            pipe = makeInstance(first);
            if (pipe == INVALID_HANDLE_VALUE) {
                reportRefused(first, ::GetLastError());
                ::WaitForSingleObject(g_StopEvent, 1000);
                continue;
            }
            first = false;
            if (refusedTries > 0) {
                Logger::warning(
                    QStringLiteral("[WorkerService] the control pipe is this "
                                   "service's again, after %1 refused tr%2 (last "
                                   "error %3)")
                        .arg(refusedTries)
                        .arg(refusedTries == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                        .arg(refusedError));
                refusedTries = 0;
            }
        }

        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ::ConnectNamedPipe(pipe, &ov) != 0;
        if (!connected) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {g_StopEvent, ov.hEvent};
                const DWORD which = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (which == WAIT_OBJECT_0 + 1) {
                    DWORD ignored = 0;
                    connected = ::GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
                } else {
                    ::CancelIoEx(pipe, &ov);
                }
            }
        }
        if (connected) serveOne(pipe);
        ::CloseHandle(ov.hEvent);
        // The next instance first, then this one goes. Stopping: nothing next.
        HANDLE next = INVALID_HANDLE_VALUE;
        if (::WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
            next = makeInstance(false);
            if (next == INVALID_HANDLE_VALUE) {
                reportRefused(false, ::GetLastError());
                // The name goes with this instance: the next one has to be the
                // first again.
                first = true;
            }
        }
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
        pipe = next;
    }
    if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
    ::LocalFree(sd);
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    const std::wstring name = WorkerService::serviceName().toStdWString();
    g_StatusHandle = ::RegisterServiceCtrlHandlerExW(name.c_str(), serviceControl, nullptr);
    if (!g_StatusHandle) return;
    report(SERVICE_START_PENDING, NO_ERROR, 3000);
    g_StopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_StopEvent) {
        report(SERVICE_STOPPED, ::GetLastError());
        return;
    }
    report(SERVICE_RUNNING);
    Logger::info("[WorkerService] listening for worker requests");
    controlLoop();
    Logger::info("[WorkerService] stopped");
    ::CloseHandle(g_StopEvent);
    g_StopEvent = nullptr;
    report(SERVICE_STOPPED);
}

#endif // Q_OS_WIN

} // namespace

namespace WorkerService {

QString serviceName()
{
    return mw::edition::productName() +
           (mw::edition::devFlag() ? QStringLiteral("-dev") : QString()) +
           QStringLiteral(" Worker");
}

QString controlPipeName()
{
    return mw::edition::productName() +
           (mw::edition::devFlag() ? QStringLiteral("-dev") : QString()) +
           QStringLiteral("-worker-service");
}

#ifdef Q_OS_WIN

bool available()
{
    // One failed launch is enough: a service that refused once refuses the same
    // way every time, and the fallbacks below it work.
    if (g_ServiceBroken.load() || ConsoleSession::launchesElsewhere()) return false;
    static const bool installed = []() {
        QString error;
        const QString image = registeredImage(&error);
        if (image.isEmpty()) {
            Logger::info("[WorkerService] no SYSTEM worker service: " + error);
            return false;
        }
        if (!ConsoleSession::isThisExecutable(image)) {
            // A build tree next to an installation: the service belongs to the
            // installed binary, and starting ITS worker from here would stream
            // the wrong code.
            Logger::info("[WorkerService] the worker service belongs to \"" + image +
                         "\", not to this executable");
            return false;
        }
        return true;
    }();
    return installed;
}

namespace {

static_assert(WorkerService::kControlPipeClientAccess == (GENERIC_READ | FILE_WRITE_DATA),
              "the control pipe is opened for reading and for writing its data alone");

/// The process the service manager runs this service as, 0 when it is not
/// running or cannot be asked. SERVICE_QUERY_STATUS is the interactive
/// user's on a service's default security.
DWORD runningServicePid()
{
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    const std::wstring name = WorkerService::serviceName().toStdWString();
    SC_HANDLE service = ::OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS);
    DWORD pid = 0;
    if (service) {
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;
        if (::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                   reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed) &&
            status.dwCurrentState == SERVICE_RUNNING)
            pid = status.dwProcessId;
        ::CloseServiceHandle(service);
    }
    ::CloseServiceHandle(scm);
    return pid;
}

/// One request on the control pipe, and its one-line answer: true on "ok …",
/// false with the reason on "err …" or when the service cannot be reached.
///
/// Identification only: the service may read who asks, never act as them.
/// And the pipe is spoken to only once its server is known to be the
/// service itself (controlPipeServerIsService).
bool askService(const QByteArray& request, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    const std::wstring path = controlPipePath();
    const DWORD access = WorkerService::kControlPipeClientAccess;
    HANDLE pipe = ::CreateFileW(path.c_str(), access, 0, nullptr, OPEN_EXISTING,
                                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_PIPE_BUSY) {
        // Another session is being started at this very moment: the service
        // serves one request at a time and each is a CreateProcess long.
        ::WaitNamedPipeW(path.c_str(), kRequestTimeoutMs);
        pipe = ::CreateFileW(path.c_str(), access, 0, nullptr, OPEN_EXISTING,
                             SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return fail(err == ERROR_FILE_NOT_FOUND
                        ? QStringLiteral("the worker service is not running")
                        : QStringLiteral("cannot reach the worker service (error %1)").arg(err));
    }
    ULONG serverPid = 0;
    const DWORD servicePid = runningServicePid();
    if (!::GetNamedPipeServerProcessId(pipe, &serverPid) ||
        !WorkerService::controlPipeServerIsService(serverPid, servicePid)) {
        ::CloseHandle(pipe);
        return fail(QStringLiteral("the worker service's pipe is served by process %1, not by "
                                   "the service (%2)")
                        .arg(serverPid)
                        .arg(servicePid));
    }

    const QByteArray line = request + '\n';
    DWORD written = 0;
    if (!::WriteFile(pipe, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(pipe);
        return fail(QStringLiteral("cannot send the request (error %1)").arg(err));
    }

    char buffer[256] = {};
    DWORD got = 0;
    const bool read = ::ReadFile(pipe, buffer, sizeof(buffer) - 1, &got, nullptr) != 0 && got > 0;
    ::CloseHandle(pipe);
    if (!read) return fail(QStringLiteral("the worker service did not answer"));

    const QString answer = QString::fromUtf8(buffer, static_cast<int>(got)).trimmed();
    if (!answer.startsWith(QStringLiteral("ok")))
        return fail(answer.startsWith(QStringLiteral("err "))
                        ? answer.mid(4)
                        : QStringLiteral("the worker service refused: ") + answer);
    return true;
}

} // namespace

bool requestWorker(const QString& base, QString* error)
{
    auto fail = [&](const QString& why) {
        g_ServiceBroken.store(true);
        if (error) *error = why;
        return false;
    };
    if (!validBase(base)) return fail(QStringLiteral("the pipe name is not a valid one"));
    QString why;
    if (!askService(base.toLatin1(), &why)) return fail(why);
    return true;
}

bool requestSignInWorker(const QString& base, QString* error)
{
    // No g_ServiceBroken: a refusal is mostly somebody at the PC, or a
    // version that does not offer it, which says nothing about the next
    // ordinary worker.
    if (!validBase(base)) {
        if (error) *error = QStringLiteral("the pipe name is not a valid one");
        return false;
    }
    return askService(QByteArray(kSignInRequestPrefix) + base.toLatin1(), error);
}

bool requestSecureAttention(QString* error)
{
    // No g_ServiceBroken here: a refused key press says nothing about whether
    // the next worker can be started.
    return askService(QByteArray(kSecureAttentionRequest), error);
}

bool requestConsoleReturn(QString* error)
{
    // No g_ServiceBroken either: a refusal here is mostly somebody signed in on
    // the PC, which says nothing about the next worker.
    return askService(QByteArray(kConsoleReturnRequest), error);
}

bool requestHide(QString* error)
{
    return askService(QByteArray(kHideRequest), error);
}

int runService()
{
    const std::wstring name = WorkerService::serviceName().toStdWString();
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(name.c_str()), serviceMain},
                                    {nullptr, nullptr}};
    if (::StartServiceCtrlDispatcherW(table)) return 0;
    const DWORD err = ::GetLastError();
    // Run by hand from a console rather than by the SCM: say so, instead of
    // exiting silently with a number nobody can place.
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        Logger::error("[WorkerService] --worker-service is started by Windows, not by hand");
    else
        Logger::error(QStringLiteral("[WorkerService] StartServiceCtrlDispatcher failed "
                                     "(error %1)")
                          .arg(err));
    return 1;
}

int installService()
{
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        Logger::error(QStringLiteral("[WorkerService] cannot open the service manager (error %1) "
                                     "— this has to run elevated")
                          .arg(::GetLastError()));
        return 1;
    }
    const std::wstring name = serviceName().toStdWString();
    const std::wstring command =
        (QLatin1Char('"') + ownExecutable() + QStringLiteral("\" --worker-service") +
         (mw::edition::devFlag() ? QStringLiteral(" --dev") : QString()))
            .toStdWString();

    // Auto-start rather than demand-start: a demand-start service would have to
    // grant SERVICE_START to the interactive user for the server to wake it,
    // and a right to start a LocalSystem process is a larger thing to hand out
    // than the few hundred kilobytes this costs while it waits on a pipe.
    SC_HANDLE service = ::OpenServiceW(scm, name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_START);
    if (service) {
        // Already there from an earlier install: point it at this {app} — an
        // update moves the executable and a stale image path starts nothing.
        if (!::ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                    SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr,
                                    nullptr, L"LocalSystem", nullptr, nullptr)) {
            Logger::error(QStringLiteral("[WorkerService] cannot update the service (error %1)")
                              .arg(::GetLastError()));
            ::CloseServiceHandle(service);
            ::CloseServiceHandle(scm);
            return 1;
        }
    } else {
        service =
            ::CreateServiceW(scm, name.c_str(), name.c_str(), SERVICE_START,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             command.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr);
        if (!service) {
            Logger::error(QStringLiteral("[WorkerService] cannot create the service (error %1)")
                              .arg(::GetLastError()));
            ::CloseServiceHandle(scm);
            return 1;
        }
        SERVICE_DESCRIPTIONW description = {};
        std::wstring text = L"Starts the " + serviceName().toStdWString() +
                            L" stream worker on the console desktop, so a remote viewer can "
                            L"answer a UAC prompt and the lock screen, and press Ctrl+Alt+Del.";
        description.lpDescription = const_cast<LPWSTR>(text.c_str());
        ::ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    }

    // Start it now; an install should not need a reboot to gain the feature.
    if (!::StartServiceW(service, 0, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING)
            Logger::warning(QStringLiteral("[WorkerService] the service is installed but did not "
                                           "start (error %1) — it will at the next boot")
                                .arg(err));
    }
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(scm);
    Logger::info("[WorkerService] installed and running");
    return 0;
}

int removeService()
{
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    const std::wstring name = serviceName().toStdWString();
    SC_HANDLE service = ::OpenServiceW(scm, name.c_str(), SERVICE_STOP | DELETE);
    if (!service) {
        ::CloseServiceHandle(scm);
        return 0; // not installed: nothing to undo
    }
    SERVICE_STATUS status = {};
    ::ControlService(service, SERVICE_CONTROL_STOP, &status);
    const bool deleted = ::DeleteService(service) != 0;
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(scm);
    if (!deleted)
        Logger::warning(QStringLiteral("[WorkerService] DeleteService failed (error %1)")
                            .arg(::GetLastError()));
    return 0;
}

#else // !Q_OS_WIN

bool available()
{
    return false;
}

bool requestWorker(const QString&, QString* error)
{
    if (error) *error = QStringLiteral("the worker service exists on Windows only");
    return false;
}

bool requestSignInWorker(const QString&, QString* error)
{
    if (error) *error = QStringLiteral("Remote Desktop sessions are a Windows notion");
    return false;
}

bool requestSecureAttention(QString* error)
{
    if (error) *error = QStringLiteral("Ctrl+Alt+Suppr is a Windows notion");
    return false;
}

bool requestConsoleReturn(QString* error)
{
    if (error) *error = QStringLiteral("Remote Desktop sessions are a Windows notion");
    return false;
}

bool requestHide(QString* error)
{
    if (error) *error = QStringLiteral("Remote Desktop sessions are a Windows notion");
    return false;
}

int runService()
{
    Logger::error("[WorkerService] the worker service exists on Windows only");
    return 1;
}

int installService()
{
    return runService();
}

int removeService()
{
    return runService();
}

#endif // Q_OS_WIN

} // namespace WorkerService
