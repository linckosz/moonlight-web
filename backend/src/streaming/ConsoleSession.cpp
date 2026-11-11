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

#include "WorkerService.h"
#include "common/Edition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRandomGenerator>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <taskschd.h>
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
/// @p preferLinked false keeps the filtered token: a program the user opens
/// themselves, never raised.
HANDLE acquireConsoleToken(ConsoleSession::Info& info, bool preferLinked = true)
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
    if (preferLinked &&
        ::GetTokenInformation(token, TokenLinkedToken, &linked, sizeof(linked), &len) &&
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

// ── The elevated worker task ────────────────────────────────────────────────

/// Once a launch through the task has failed, every later one would fail the
/// same way after the same 10 s wait: stop offering it for this run.
std::atomic<bool> g_TaskLaunchBroken{false};

QString ownExecutable()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

/// Full image path of a process, or empty. PROCESS_QUERY_LIMITED_INFORMATION is
/// granted across integrity levels, so this works on an elevated worker too —
/// and on the SYSTEM worker only because the service writes its DACL for the
/// user (WorkerService, launchWorker): left to the SYSTEM default, it answers
/// "" to a server that is not elevated.
QString processImage(DWORD pid)
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

bool sameFile(const QString& a, const QString& b)
{
    return !a.isEmpty() && QDir::toNativeSeparators(a).compare(QDir::toNativeSeparators(b),
                                                               Qt::CaseInsensitive) == 0;
}

/// Release-on-scope for the handful of Task Scheduler interfaces used here.
template <typename T> struct Com
{
    T* p = nullptr;
    ~Com()
    {
        if (p) p->Release();
    }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct ComApartment
{
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment()
    {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

/// The registered task by name, or null. `service` must outlive the result.
bool openTask(const QString& name, Com<ITaskService>& service, Com<IRegisteredTask>& task)
{
    if (FAILED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.out()))))
        return false;
    VARIANT none;
    ::VariantInit(&none);
    if (FAILED(service->Connect(none, none, none, none))) return false;
    Com<ITaskFolder> root;
    BSTR rootPath = ::SysAllocString(L"\\");
    const HRESULT hr = service->GetFolder(rootPath, root.out());
    ::SysFreeString(rootPath);
    if (FAILED(hr)) return false;
    BSTR taskName = ::SysAllocString(reinterpret_cast<const OLECHAR*>(name.utf16()));
    const HRESULT got = root->GetTask(taskName, task.out());
    ::SysFreeString(taskName);
    return SUCCEEDED(got);
}

/// The executable the task's first action runs, unquoted, or empty.
QString taskExecutable(IRegisteredTask* task)
{
    Com<ITaskDefinition> definition;
    Com<IActionCollection> actions;
    Com<IAction> action;
    Com<IExecAction> exec;
    if (FAILED(task->get_Definition(definition.out())) ||
        FAILED(definition->get_Actions(actions.out())) ||
        FAILED(actions->get_Item(1, action.out())) ||
        FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.out()))))
        return {};
    BSTR path = nullptr;
    if (FAILED(exec->get_Path(&path)) || !path) return {};
    QString result = QString::fromWCharArray(path).trimmed();
    ::SysFreeString(path);
    if (result.size() >= 2 && result.startsWith(QLatin1Char('"')) &&
        result.endsWith(QLatin1Char('"')))
        result = result.mid(1, result.size() - 2);
    return result;
}

/// Start the registered task `taskName`, handing it `base` as its $(Arg0) —
/// the only thing that travels, the fixed arguments being in the task itself.
bool runWorkerTask(const QString& taskName, const QString& base, QString* error)
{
    auto fail = [&](const QString& why) {
        *error = why;
        return false;
    };
    ComApartment com;
    Com<ITaskService> service;
    Com<IRegisteredTask> task;
    if (!com.usable() || !openTask(taskName, service, task))
        return fail(QStringLiteral("task \"%1\" not found").arg(taskName));

    SAFEARRAY* params = ::SafeArrayCreateVector(VT_BSTR, 0, 1);
    LONG index = 0;
    BSTR arg = ::SysAllocString(reinterpret_cast<const OLECHAR*>(base.utf16()));
    ::SafeArrayPutElement(params, &index, arg); // copies
    ::SysFreeString(arg);
    VARIANT variant;
    ::VariantInit(&variant);
    variant.vt = VT_ARRAY | VT_BSTR;
    variant.parray = params;
    Com<IRunningTask> running;
    const HRESULT hr = task->RunEx(variant, TASK_RUN_NO_FLAGS, 0, nullptr, running.out());
    ::VariantClear(&variant);
    if (FAILED(hr))
        return fail(QStringLiteral("running task \"%1\" failed (0x%2)")
                        .arg(taskName)
                        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
    return true;
}

/// "D:P(A;;GA;;;<our user SID>)": the pipes admit the user this process runs
/// as — which an elevated process of theirs still is — and nobody else. A
/// default pipe DACL would also let Everyone read.
///
/// @p admitSystem adds "(A;;GA;;;SY)", for the worker the launcher service
/// starts: it runs as SYSTEM, which the user's own ACE does not cover, and
/// without the entry it could not open its own stdin. SYSTEM can help itself to
/// any handle on the machine anyway — the ACE grants nothing it did not have,
/// it only saves it the trouble.
bool userOnlySecurity(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd, bool admitSystem)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    LPWSTR sid = nullptr;
    const bool gotSid =
        size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid);
    ::CloseHandle(token);
    if (!gotSid) return false;
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    ::LocalFree(sid);
    if (admitSystem) sddl += L"(A;;GA;;;SY)";
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                                nullptr))
        return false;
    sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

std::wstring pipePath(const QString& base, const char* which)
{
    return (QStringLiteral("\\\\.\\pipe\\") + base + QLatin1Char('-') + QLatin1String(which))
        .toStdWString();
}

/// ConnectNamedPipe with a deadline. The pipe is overlapped for this alone.
bool awaitClient(HANDLE pipe, std::chrono::steady_clock::time_point deadline)
{
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    bool connected = false;
    if (::ConnectNamedPipe(pipe, &ov)) {
        connected = true;
    } else {
        const DWORD err = ::GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const DWORD wait = left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
            DWORD ignored = 0;
            if (::WaitForSingleObject(ov.hEvent, wait) == WAIT_OBJECT_0) {
                connected = ::GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
            } else {
                ::CancelIoEx(pipe, &ov);
                ::GetOverlappedResult(pipe, &ov, &ignored, TRUE);
            }
        }
    }
    ::CloseHandle(ov.hEvent);
    return connected;
}

/// One ReadFile/WriteFile that works on both kinds of handle this file holds:
/// the anonymous pipes (synchronous) and the named ones (overlapped).
bool pipeIo(HANDLE handle, bool overlapped, bool write, void* data, DWORD size, DWORD* done)
{
    if (!overlapped) {
        return write ? ::WriteFile(handle, data, size, done, nullptr) != 0
                     : ::ReadFile(handle, data, size, done, nullptr) != 0;
    }
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    BOOL ok = write ? ::WriteFile(handle, data, size, nullptr, &ov)
                    : ::ReadFile(handle, data, size, nullptr, &ov);
    if (ok || ::GetLastError() == ERROR_IO_PENDING)
        ok = ::GetOverlappedResult(handle, &ov, done, TRUE);
    ::CloseHandle(ov.hEvent);
    return ok != 0;
}

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

QString elevatedWorkerTaskName()
{
    return mw::edition::productName() +
           (mw::edition::devFlag() ? QStringLiteral("-dev") : QString()) +
           QStringLiteral(" Stream Worker");
}

bool elevatedWorkerAvailable()
{
#ifdef Q_OS_WIN
    if (g_TaskLaunchBroken.load() || launchesElsewhere()) return false;
    // Already elevated (started "as administrator"): the plain child inherits
    // that, nothing to gain from a detour.
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation = {};
        DWORD size = 0;
        const bool elevated =
            ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
            elevation.TokenIsElevated;
        ::CloseHandle(token);
        if (elevated) return false;
    }
    ComApartment com;
    if (!com.usable()) return false;
    Com<ITaskService> service;
    Com<IRegisteredTask> task;
    if (!openTask(elevatedWorkerTaskName(), service, task)) return false;
    return sameFile(taskExecutable(task.p), ownExecutable());
#else
    return false;
#endif
}

QString processImage(quint32 pid)
{
#ifdef Q_OS_WIN
    return ::processImage(static_cast<DWORD>(pid));
#else
    Q_UNUSED(pid);
    return {};
#endif
}

bool isThisExecutable(const QString& image)
{
#ifdef Q_OS_WIN
    return sameFile(image, ownExecutable());
#else
    Q_UNUSED(image);
    return false;
#endif
}

namespace {
std::atomic<quint32> g_WorkerServerPid{0};
}

quint32 workerServerPid()
{
    return g_WorkerServerPid.load();
}

bool attachWorkerPipes(const QString& base, QString* error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(base);
    if (error) *error = QStringLiteral("worker pipes exist on Windows only");
    return false;
#else
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    struct End
    {
        const char* which;
        DWORD access;
        int fd;
        DWORD std;
        HANDLE handle = INVALID_HANDLE_VALUE;
    } ends[] = {{"in", GENERIC_READ, 0, STD_INPUT_HANDLE},
                {"out", GENERIC_WRITE, 1, STD_OUTPUT_HANDLE},
                {"err", GENERIC_WRITE, 2, STD_ERROR_HANDLE}};

    for (End& end : ends) {
        // SECURITY_IDENTIFICATION: the server may learn who we are, not act as
        // us — it is the unelevated side, and we are the elevated one.
        end.handle =
            ::CreateFileW(pipePath(base, end.which).c_str(), end.access, 0, nullptr, OPEN_EXISTING,
                          SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (end.handle == INVALID_HANDLE_VALUE)
            return fail(QStringLiteral("cannot open pipe \"%1\" (error %2)")
                            .arg(QLatin1String(end.which))
                            .arg(::GetLastError()));
    }

    // The task can be started by any process of this user, with any pipe name.
    // Only our own server gets a worker that types as administrator.
    ULONG serverPid = 0;
    if (!::GetNamedPipeServerProcessId(ends[0].handle, &serverPid))
        return fail(
            QStringLiteral("cannot identify the pipe server (error %1)").arg(::GetLastError()));
    const QString serverImage = processImage(serverPid);
    if (!sameFile(serverImage, ownExecutable()))
        return fail(QStringLiteral("pipe server is \"%1\" (pid %2), not this executable")
                        .arg(serverImage)
                        .arg(serverPid));
    g_WorkerServerPid = serverPid;

    // A task runs a console-subsystem build (a dev tree) with a console of its
    // own; nothing is ever shown in it.
    if (::GetConsoleWindow()) ::FreeConsole();

    // A windowless build starts with no standard handles: fds 0-2 are free and
    // the CRT streams point at nothing. Give the three streams a real fd first
    // (NUL), so the pipes' own fds land above them, then put each pipe under
    // its stream's fd — std::cin, stdout and std::cerr follow.
    FILE* streams[] = {stdin, stdout, stderr};
    for (End& end : ends) {
        FILE* stream = streams[end.fd];
        if (_fileno(stream) < 0 && !std::freopen("NUL", end.fd == 0 ? "rb" : "wb", stream))
            return fail(QStringLiteral("cannot reopen std stream %1").arg(end.fd));
    }
    for (End& end : ends) {
        FILE* stream = streams[end.fd];
        const int target = _fileno(stream);
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(end.handle),
                                       (end.fd == 0 ? _O_RDONLY : _O_WRONLY) | _O_BINARY);
        if (fd < 0 || target < 0 || _dup2(fd, target) != 0)
            return fail(QStringLiteral("cannot make pipe \"%1\" fd %2")
                            .arg(QLatin1String(end.which))
                            .arg(target));
        _close(fd); // closes our copy of the handle; `target` holds its own
        ::SetStdHandle(end.std, reinterpret_cast<HANDLE>(_get_osfhandle(target)));
        std::clearerr(stream);
    }
    return true;
#endif
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
    /// The pipes are named and overlapped (startThroughTask), not anonymous.
    bool overlapped = false;
    /// The process handle carries no PROCESS_TERMINATE (an elevated worker):
    /// kill() closes stdin instead.
    bool canTerminate = true;
    std::thread outReader;
    std::thread errReader;
    std::thread waiter;
#endif
    std::atomic<bool> running{false};
    std::mutex writeMutex;
    QString user;
};

bool ConsoleSession::startAsConsoleUser(const QString& program, const QStringList& args,
                                        QString* error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(program);
    Q_UNUSED(args);
    if (error) *error = QStringLiteral("console-session launch exists on Windows only");
    return false;
#else
    ConsoleSession::Info info;
    HANDLE token = acquireConsoleToken(info, /*preferLinked=*/false);
    if (!token) {
        if (error) *error = info.reason;
        return false;
    }
    // The user's own environment and desktop; no handle of ours crosses over.
    LPVOID env = nullptr;
    if (!::CreateEnvironmentBlock(&env, token, FALSE)) env = nullptr;
    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = desktop;
    QString commandLine = quoteArg(program);
    for (const QString& arg : args)
        commandLine += QLatin1Char(' ') + quoteArg(arg);
    std::wstring cmd = commandLine.toStdWString();
    const std::wstring app = program.toStdWString();
    const std::wstring cwd = QFileInfo(program).absolutePath().toStdWString();
    PROCESS_INFORMATION pi = {};
    const BOOL created = ::CreateProcessAsUserW(
        token, app.c_str(), cmd.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, cwd.c_str(), &si, &pi);
    const DWORD createError = created ? 0 : ::GetLastError();
    if (env) ::DestroyEnvironmentBlock(env);
    ::CloseHandle(token);
    if (!created) {
        if (error)
            *error = QStringLiteral("CreateProcessAsUser failed (error %1)").arg(createError);
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
#endif
}

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

    beginDraining();
    return true;
#endif
}

bool ConsoleProcess::startThroughTask(const QString& taskName, QString* error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(taskName);
    if (error) *error = QStringLiteral("task launch exists on Windows only");
    return false;
#else
    const bool started = startWithNamedPipes(
        /*systemWorker=*/false,
        [&taskName](const QString& base, QString* why) {
            return runWorkerTask(taskName, base, why);
        },
        error);
    if (!started) g_TaskLaunchBroken.store(true);
    return started;
#endif
}

bool ConsoleProcess::startThroughService(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("the worker service exists on Windows only");
    return false;
#else
    return startWithNamedPipes(
        /*systemWorker=*/true,
        [](const QString& base, QString* why) { return WorkerService::requestWorker(base, why); },
        error);
#endif
}

bool ConsoleProcess::startThroughSignInService(QString* error)
{
#ifndef Q_OS_WIN
    if (error) *error = QStringLiteral("the worker service exists on Windows only");
    return false;
#else
    return startWithNamedPipes(
        /*systemWorker=*/true,
        [](const QString& base, QString* why) {
            return WorkerService::requestSignInWorker(base, why);
        },
        error);
#endif
}

bool ConsoleProcess::startWithNamedPipes(
    bool systemWorker, const std::function<bool(const QString&, QString*)>& launch, QString* error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(systemWorker);
    Q_UNUSED(launch);
    if (error) *error = QStringLiteral("named-pipe launch exists on Windows only");
    return false;
#else
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (d->process) return fail(QStringLiteral("already started"));

    // ── Three named pipes, the user's alone ─────────────────────────────────
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!userOnlySecurity(sa, sd, systemWorker))
        return fail(QStringLiteral("cannot build the pipe DACL (error %1)").arg(::GetLastError()));
    const quint64 nonce[2] = {QRandomGenerator::system()->generate64(),
                              QRandomGenerator::system()->generate64()};
    const QString base = QStringLiteral("%1-worker-%2-%3%4")
                             .arg(mw::edition::productName())
                             .arg(::GetCurrentProcessId())
                             .arg(nonce[0], 16, 16, QLatin1Char('0'))
                             .arg(nonce[1], 16, 16, QLatin1Char('0'));
    // FIRST_PIPE_INSTANCE + a single instance: nobody can have squatted the name,
    // nobody can open a second end of it. Buffer sizes as for the anonymous
    // pipes: a whole config fits in stdin before the worker has read a byte.
    auto makePipe = [&](const char* which, DWORD direction, DWORD outSize, DWORD inSize) {
        return ::CreateNamedPipeW(pipePath(base, which).c_str(),
                                  direction | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                      PIPE_REJECT_REMOTE_CLIENTS,
                                  1, outSize, inSize, 0, &sa);
    };
    HANDLE in = makePipe("in", PIPE_ACCESS_OUTBOUND, 1 << 20, 0);
    HANDLE out = makePipe("out", PIPE_ACCESS_INBOUND, 0, 1 << 16);
    HANDLE err = makePipe("err", PIPE_ACCESS_INBOUND, 0, 1 << 16);
    const DWORD pipeError = ::GetLastError();
    ::LocalFree(sd);
    auto closePipes = [&]() {
        for (HANDLE h : {in, out, err})
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    };
    if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
        closePipes();
        return fail(QStringLiteral("CreateNamedPipe failed (error %1)").arg(pipeError));
    }

    // ── Have somebody start a worker on that name ───────────────────────────
    // The task scheduler or the launcher service; from here on the two are the
    // same thing — something that runs this executable somewhere we cannot.
    {
        QString why;
        if (!launch(base, &why)) {
            closePipes();
            return fail(why);
        }
    }

    // ── Wait for the worker on all three ─────────────────────────────────────
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (HANDLE h : {in, out, err}) {
        if (!awaitClient(h, deadline)) {
            closePipes();
            return fail(QStringLiteral("the worker never connected"));
        }
    }
    ULONG pid = 0;
    ::GetNamedPipeClientProcessId(in, &pid);
    const QString image = processImage(pid);
    if (!sameFile(image, ownExecutable())) {
        closePipes();
        return fail(QStringLiteral("pipe client is \"%1\" (pid %2), not this executable")
                        .arg(image)
                        .arg(pid));
    }
    // Whether TERMINATE is granted on an elevated worker depends on who created
    // it (Task Scheduler's do grant it); ask, and fall back to closing stdin.
    HANDLE process = ::OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    d->canTerminate = process != nullptr;
    if (!process)
        process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        closePipes();
        return fail(
            QStringLiteral("cannot open the worker process (error %1)").arg(::GetLastError()));
    }

    d->process = process;
    d->pid = pid;
    d->overlapped = true;
    d->stdinWrite = in;
    d->stdoutRead = out;
    d->stderrRead = err;
    // What the launch actually gave: a standard user's HighestAvailable is
    // their ordinary token, and the log should say so rather than promise more.
    bool elevated = false;
    HANDLE workerToken = nullptr;
    if (::OpenProcessToken(process, TOKEN_QUERY, &workerToken)) {
        TOKEN_ELEVATION elevation = {};
        DWORD size = 0;
        elevated = ::GetTokenInformation(workerToken, TokenElevation, &elevation, sizeof(elevation),
                                         &size) &&
                   elevation.TokenIsElevated;
        ::CloseHandle(workerToken);
    }
    if (systemWorker)
        d->user = QStringLiteral("SYSTEM (launcher service)");
    else
        d->user = elevated ? QStringLiteral("this user, elevated (task)")
                           : QStringLiteral("this user, not elevated (task)");
    d->running.store(true);
    beginDraining();
    return true;
#endif
}

void ConsoleProcess::beginDraining()
{
#ifdef Q_OS_WIN
    auto reader = [this](HANDLE handle, bool isStderr) {
        std::vector<char> buffer(8192);
        DWORD got = 0;
        while (pipeIo(handle, d->overlapped, false, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &got) &&
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
        if (!pipeIo(d->stdinWrite, d->overlapped, true, const_cast<char*>(p), left, &written))
            return;
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
    if (!d->process || !d->running.load()) return;
    if (d->canTerminate && ::TerminateProcess(d->process, 0xF291)) return;
    // Out of our reach: end-of-file on stdin is the worker's own cue to tear
    // down and exit ("the parent is gone").
    std::lock_guard<std::mutex> lock(d->writeMutex);
    if (d->stdinWrite) {
        ::CloseHandle(d->stdinWrite);
        d->stdinWrite = nullptr;
    }
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
