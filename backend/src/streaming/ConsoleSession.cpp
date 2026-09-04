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

#include "ConsoleSession.h"

#include <QFileInfo>
#include <QMetaObject>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>
#endif

namespace {

bool forcedForTesting()
{
    return qEnvironmentVariable("MW_CONSOLE_LAUNCH") == QLatin1String("force");
}

#ifdef Q_OS_WIN

quint32 ownSessionId()
{
    DWORD sessionId = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId)) return 0;
    return sessionId;
}

QString sessionUserName(DWORD sessionId)
{
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    QString name;
    if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &buffer,
                                      &bytes) &&
        buffer) {
        name = QString::fromWCharArray(buffer);
    }
    if (buffer) ::WTSFreeMemory(buffer);
    return name;
}

/// The token a console launch runs under, or null with `info.reason` filled.
///
/// WTSQueryUserToken hands back the token the user's own shell runs as, which
/// under UAC is the FILTERED one for an administrator. The linked token — the
/// full one an elevated program gets — is asked for next, and preferred when
/// it exists: the worker then types into elevated windows the way the user
/// could, and still runs as the user, not as SYSTEM. Both need SeTcbPrivilege
/// to come back as primary tokens, which a LocalSystem service has.
HANDLE acquireConsoleToken(ConsoleSession::Info& info)
{
    const DWORD sessionId = ::WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        info.reason = QStringLiteral(
            "no console session (no display attached, or Windows is between sessions)");
        return nullptr;
    }
    info.sessionId = sessionId;

    HANDLE token = nullptr;
    if (!::WTSQueryUserToken(sessionId, &token)) {
        const DWORD err = ::GetLastError();
        switch (err) {
        case ERROR_NO_TOKEN:
            info.reason = QStringLiteral("nobody is logged on at the console");
            break;
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_ACCESS_DENIED:
            info.reason = QStringLiteral("this process may not enter the console session — the "
                                         "service has to run as LocalSystem");
            break;
        default:
            info.reason = QStringLiteral("WTSQueryUserToken failed (error %1)").arg(err);
            break;
        }
        return nullptr;
    }
    info.userPresent = true;
    info.userName = sessionUserName(sessionId);

    TOKEN_LINKED_TOKEN linked = {};
    DWORD len = 0;
    if (::GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &len) &&
        linked.LinkedToken) {
        ::CloseHandle(token);
        token = linked.LinkedToken;
        info.elevated = true;
    }

    // Whatever came back, make it a primary token of our own: CreateProcessAsUser
    // wants nothing else, and the linked token's type depends on who asked.
    HANDLE primary = nullptr;
    if (!::DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary,
                            &primary)) {
        info.reason = QStringLiteral("DuplicateTokenEx failed (error %1)").arg(::GetLastError());
        ::CloseHandle(token);
        return nullptr;
    }
    ::CloseHandle(token);
    return primary;
}

QString quoteArg(const QString& arg)
{
    if (!arg.contains(QLatin1Char(' ')) && !arg.contains(QLatin1Char('"'))) return arg;
    QString quoted = arg;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

struct Pipe
{
    HANDLE read = nullptr;
    HANDLE write = nullptr;

    /// Both ends inheritable at creation; the parent's end is made private
    /// right after, so that only the child's end can travel.
    bool create(DWORD size, bool parentKeepsRead)
    {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!::CreatePipe(&read, &write, &sa, size)) return false;
        return ::SetHandleInformation(parentKeepsRead ? read : write, HANDLE_FLAG_INHERIT, 0) != 0;
    }

    void closeBoth()
    {
        if (read) ::CloseHandle(read);
        if (write) ::CloseHandle(write);
        read = write = nullptr;
    }
};

#endif // Q_OS_WIN

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ConsoleSession
// ─────────────────────────────────────────────────────────────────────────────

namespace ConsoleSession {

bool launchesElsewhere()
{
#ifdef Q_OS_WIN
    static const bool elsewhere = forcedForTesting() || ownSessionId() == 0;
    return elsewhere;
#else
    return false;
#endif
}

Info query()
{
    Info info;
    info.elsewhere = launchesElsewhere();
#ifdef Q_OS_WIN
    if (forcedForTesting() && ownSessionId() != 0) {
        // Testing from a desktop: the "console user" is ourselves.
        info.userPresent = true;
        info.sessionId = ownSessionId();
        info.userName = QStringLiteral("(this session)");
        return info;
    }
    HANDLE token = acquireConsoleToken(info);
    if (token) ::CloseHandle(token);
#else
    info.reason = QStringLiteral("console-session launch exists on Windows only");
#endif
    return info;
}

} // namespace ConsoleSession

// ─────────────────────────────────────────────────────────────────────────────
// ConsoleProcess
// ─────────────────────────────────────────────────────────────────────────────

struct ConsoleProcess::Impl
{
#ifdef Q_OS_WIN
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrRead = nullptr;
    DWORD pid = 0;
    std::thread outReader;
    std::thread errReader;
    std::thread waiter;
#endif
    std::atomic<bool> running{false};
    std::mutex writeMutex;
    QString user;
};

ConsoleProcess::ConsoleProcess(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{}

ConsoleProcess::~ConsoleProcess()
{
#ifdef Q_OS_WIN
    if (d->running.load()) kill();
    // The waiter joins both readers itself, once the process is gone and the
    // pipes have broken — so joining it is joining everything.
    if (d->waiter.joinable()) d->waiter.join();
    if (d->outReader.joinable()) d->outReader.join();
    if (d->errReader.joinable()) d->errReader.join();
    if (d->stdinWrite) ::CloseHandle(d->stdinWrite);
    if (d->stdoutRead) ::CloseHandle(d->stdoutRead);
    if (d->stderrRead) ::CloseHandle(d->stderrRead);
    if (d->process) ::CloseHandle(d->process);
#endif
}

bool ConsoleProcess::start(const QString& program, const QStringList& args, QString* error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(program);
    Q_UNUSED(args);
    if (error) *error = QStringLiteral("console-session launch exists on Windows only");
    return false;
#else
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (d->process) return fail(QStringLiteral("already started"));

    // ── Who the child runs as ────────────────────────────────────────────────
    ConsoleSession::Info info;
    HANDLE token = nullptr;
    const bool sameSession = forcedForTesting() && ownSessionId() != 0;
    if (sameSession) {
        // Testing from a desktop: no token, plain CreateProcess, everything
        // else — pipes, handle list, environment, desktop — exactly as below.
        d->user = QStringLiteral("(this session)");
    } else {
        token = acquireConsoleToken(info);
        if (!token) return fail(info.reason);
        d->user = info.userName +
                  (info.elevated ? QStringLiteral(" (elevated)") : QStringLiteral(" (standard)"));
    }

    // ── Three pipes; the child's ends are the only handles it inherits ─────
    // The parent may be SYSTEM and the child is the user: every other handle
    // this process holds — sockets, the settings file, the log — must not cross
    // that line, which PROC_THREAD_ATTRIBUTE_HANDLE_LIST guarantees.
    Pipe in, out, err;
    // stdin carries the session config on one line (a few KB, certificates
    // included) and must never block the parent's event loop: size it so that
    // a whole config fits before the child has read a byte.
    if (!in.create(1 << 20, false) || !out.create(1 << 16, true) || !err.create(1 << 16, true)) {
        in.closeBoth();
        out.closeBoth();
        err.closeBoth();
        if (token) ::CloseHandle(token);
        return fail(QStringLiteral("CreatePipe failed (error %1)").arg(::GetLastError()));
    }

    HANDLE inherit[3] = {in.read, out.write, err.write};
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrStorage(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    bool attrsOk = ::InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != 0;
    if (attrsOk) {
        attrsOk = ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                              sizeof(inherit), nullptr, nullptr) != 0;
    }

    // ── Environment and desktop ──────────────────────────────────────────────
    // The user's own environment (their %APPDATA%, %USERPROFILE%, PATH), not
    // the service's: that is where Qt then puts the worker's log and identity.
    LPVOID env = nullptr;
    if (token && !::CreateEnvironmentBlock(&env, token, FALSE)) env = nullptr;

    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.lpDesktop = desktop;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in.read;
    si.StartupInfo.hStdOutput = out.write;
    si.StartupInfo.hStdError = err.write;
    si.lpAttributeList = attrsOk ? attrs : nullptr;

    QString commandLine = quoteArg(program);
    for (const QString& arg : args)
        commandLine += QLatin1Char(' ') + quoteArg(arg);
    std::wstring cmd = commandLine.toStdWString();
    const std::wstring app = program.toStdWString();
    const std::wstring cwd = QFileInfo(program).absolutePath().toStdWString();

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    if (attrsOk) flags |= EXTENDED_STARTUPINFO_PRESENT;

    PROCESS_INFORMATION pi = {};
    BOOL created;
    if (token) {
        created = ::CreateProcessAsUserW(token, app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                         flags, env, cwd.c_str(), &si.StartupInfo, &pi);
    } else {
        created = ::CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, env,
                                   cwd.c_str(), &si.StartupInfo, &pi);
    }
    const DWORD createError = created ? 0 : ::GetLastError();

    if (env) ::DestroyEnvironmentBlock(env);
    if (attrsOk) ::DeleteProcThreadAttributeList(attrs);
    if (token) ::CloseHandle(token);
    // The child holds its own copies now; ours would only keep the pipes from
    // ever reporting end-of-file.
    ::CloseHandle(in.read);
    ::CloseHandle(out.write);
    ::CloseHandle(err.write);

    if (!created) {
        ::CloseHandle(in.write);
        ::CloseHandle(out.read);
        ::CloseHandle(err.read);
        return fail(QStringLiteral("%1 failed (error %2)")
                        .arg(token ? QStringLiteral("CreateProcessAsUser")
                                   : QStringLiteral("CreateProcess"))
                        .arg(createError));
    }
    ::CloseHandle(pi.hThread);

    d->process = pi.hProcess;
    d->pid = pi.dwProcessId;
    d->stdinWrite = in.write;
    d->stdoutRead = out.read;
    d->stderrRead = err.read;
    d->running.store(true);

    // ── Drain the pipes on threads of their own ─────────────────────────────
    auto reader = [this](HANDLE handle, bool isStderr) {
        std::vector<char> buffer(8192);
        DWORD got = 0;
        while (
            ::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) &&
            got > 0) {
            const QByteArray data(buffer.data(), static_cast<int>(got));
            QMetaObject::invokeMethod(
                this,
                [this, data, isStderr]() {
                    if (isStderr)
                        emit stderrData(data);
                    else
                        emit stdoutData(data);
                },
                Qt::QueuedConnection);
        }
    };
    d->outReader = std::thread(reader, d->stdoutRead, false);
    d->errReader = std::thread(reader, d->stderrRead, true);

    // Exit is reported only once both readers are done: the pipes break when
    // the child dies, the readers deliver what was left, and a "response" the
    // child wrote on its way out is therefore never overtaken by its exit.
    d->waiter = std::thread([this]() {
        ::WaitForSingleObject(d->process, INFINITE);
        if (d->outReader.joinable()) d->outReader.join();
        if (d->errReader.joinable()) d->errReader.join();
        DWORD code = 0;
        ::GetExitCodeProcess(d->process, &code);
        d->running.store(false);
        // An NTSTATUS failure (0xC0000005 …) is a crash, a small number is a
        // return value — the same reading QProcess makes.
        const bool crashed = code >= 0x80000000u;
        QMetaObject::invokeMethod(
            this, [this, code, crashed]() { emit finished(static_cast<int>(code), crashed); },
            Qt::QueuedConnection);
    });

    return true;
#endif
}

void ConsoleProcess::write(const QByteArray& data)
{
#ifdef Q_OS_WIN
    std::lock_guard<std::mutex> lock(d->writeMutex);
    if (!d->stdinWrite || !d->running.load()) return;
    const char* p = data.constData();
    DWORD left = static_cast<DWORD>(data.size());
    while (left > 0) {
        DWORD written = 0;
        if (!::WriteFile(d->stdinWrite, p, left, &written, nullptr)) return;
        p += written;
        left -= written;
    }
#else
    Q_UNUSED(data);
#endif
}

bool ConsoleProcess::isRunning() const
{
    return d->running.load();
}

void ConsoleProcess::kill()
{
#ifdef Q_OS_WIN
    if (d->process && d->running.load()) ::TerminateProcess(d->process, 0xF291);
#endif
}

qint64 ConsoleProcess::processId() const
{
#ifdef Q_OS_WIN
    return d->pid;
#else
    return 0;
#endif
}

QString ConsoleProcess::userName() const
{
    return d->user;
}
