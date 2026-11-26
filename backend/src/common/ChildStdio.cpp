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

#include "ChildStdio.h"

#include <QVariant>
#include <QtDebug>

#include <algorithm>
#include <atomic>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <climits>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace mw {

namespace {

std::atomic<bool> g_RefuseNext{false};

/// The property that says a QProcess already goes through passOnlyStdio.
constexpr const char* kSet = "mwPassOnlyStdio";

bool alreadySet(QProcess& process)
{
    if (process.property(kSet).toBool()) return true;
    process.setProperty(kSet, true);
    return false;
}

#ifdef Q_OS_WIN

/// What one start of the child needs: the attribute list (its buffer is
/// sized once, here) and the extended startup information that points to it.
/// Owned by the modifier, so it lives as long as the QProcess keeps it.
struct HandleList
{
    std::vector<unsigned char> buffer;
    bool initialized = false;
    STARTUPINFOEXW extended{};
    HANDLE handles[3] = {};
    DWORD count = 0;
    /// The last start was refused, with this code
    bool refused = false;
    DWORD code = 0;

    HandleList()
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        buffer.resize(size);
    }
    ~HandleList() { clear(); }
    HandleList(const HandleList&) = delete;
    HandleList& operator=(const HandleList&) = delete;

    LPPROC_THREAD_ATTRIBUTE_LIST list()
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    }
    void clear()
    {
        if (initialized) DeleteProcThreadAttributeList(list());
        initialized = false;
    }
    void add(HANDLE h)
    {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
        // A handle that cannot be inherited would not reach the child anyway,
        // and CreateProcess refuses a list that names one
        DWORD flags = 0;
        if (!GetHandleInformation(h, &flags) || !(flags & HANDLE_FLAG_INHERIT)) return;
        for (DWORD i = 0; i < count; ++i)
            if (handles[i] == h) return; // stdout and stderr merged
        handles[count++] = h;
    }
};

/// The child is not created. What carries it is the empty program name:
/// CreateProcess refuses the start, QProcess reports FailedToStart and never
/// emits started(). The rest is a second line that does not count on how
/// QProcess goes on after CreateProcess: no handle may pass, the size is one
/// CreateProcess does not accept, and a child created all the same would be
/// created suspended and ended as soon as started() says it exists.
void refuse(QProcess::CreateProcessArguments* arguments, HandleList& state, DWORD code)
{
    state.refused = true;
    state.code = code;
    arguments->inheritHandles = FALSE;
    arguments->flags |= CREATE_SUSPENDED;
    arguments->startupInfo->cb = 0;
    arguments->applicationName = L"";
}

void modify(QProcess::CreateProcessArguments* arguments, HandleList& state)
{
    state.clear();
    state.refused = false;
    state.code = 0;
    state.count = 0;

    const STARTUPINFOW& given = *arguments->startupInfo;
    if (given.dwFlags & STARTF_USESTDHANDLES) {
        state.add(given.hStdInput);
        state.add(given.hStdOutput);
        state.add(given.hStdError);
    }
    if (state.count == 0) {
        // No standard channel to pass: nothing is inherited at all
        arguments->inheritHandles = FALSE;
        return;
    }
    if (g_RefuseNext.exchange(false)) {
        refuse(arguments, state, ERROR_INVALID_PARAMETER);
        return;
    }
    SIZE_T size = state.buffer.size();
    if (state.buffer.empty() || !InitializeProcThreadAttributeList(state.list(), 1, 0, &size)) {
        refuse(arguments, state, GetLastError());
        return;
    }
    state.initialized = true;
    if (!UpdateProcThreadAttribute(state.list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   state.handles, state.count * sizeof(HANDLE), nullptr, nullptr)) {
        refuse(arguments, state, GetLastError());
        return;
    }
    state.extended.StartupInfo = given;
    state.extended.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    state.extended.lpAttributeList = state.list();
    arguments->startupInfo = &state.extended.StartupInfo;
    arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
    arguments->inheritHandles = TRUE;
}

/// Once per refused start: the warning with the system's code.
void tellRefusal(HandleList& state)
{
    state.refused = false;
    qWarning().nospace() << "[ChildStdio] a child was not started: its handle list could not be "
                            "made (error "
                         << state.code << ")";
}

/// A refused start that still made a child: it is ended before it runs. Its
/// process is found by the id QProcess gives while the child is running,
/// never through what QProcess keeps of CreateProcess's answer.
void endRefused(QProcess& process)
{
    const qint64 id = process.processId();
    if (id <= 0) return;
    const HANDLE child =
        OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(id));
    if (!child) return;
    TerminateProcess(child, ERROR_ACCESS_DENIED);
    WaitForSingleObject(child, 5000);
    CloseHandle(child);
}

#else

/// What the child needs, ready before the fork: the descriptors not to mark,
/// in order, and the bound of the loop when close_range is not there.
struct Plan
{
    std::vector<int> leftOpen;
    int bound = 0;
    std::function<void()> then;
};

int descriptorBound()
{
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur <= static_cast<rlim_t>(INT_MAX))
        return static_cast<int>(limit.rlim_cur);
    const long open = sysconf(_SC_OPEN_MAX);
    if (open > 0 && open <= INT_MAX) return static_cast<int>(open);
    return 65536;
}

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

/// Marks close-on-exec every open descriptor from @p low to @p high
/// (included; -1 = to the end). Runs in the child: system calls only.
void markRange(int low, int high, int bound) noexcept
{
#if defined(__linux__) && defined(SYS_close_range)
    const unsigned int last = high < 0 ? ~0U : static_cast<unsigned int>(high);
    if (::syscall(SYS_close_range, static_cast<unsigned int>(low), last, CLOSE_RANGE_CLOEXEC) == 0)
        return;
#endif
    const int end = high < 0 ? bound - 1 : std::min(high, bound - 1);
    for (int fd = low; fd <= end; ++fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags != -1 && !(flags & FD_CLOEXEC)) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void markInChild(const Plan& plan) noexcept
{
    int low = 3;
    for (const int open : plan.leftOpen) {
        if (open > low) markRange(low, open - 1, plan.bound);
        low = open + 1;
    }
    markRange(low, -1, plan.bound);
}

#endif

} // namespace

void passOnlyStdio(QProcess& process)
{
#ifdef Q_OS_WIN
    if (alreadySet(process)) return;
    auto state = std::make_shared<HandleList>();
    process.setCreateProcessArgumentsModifier(
        [state](QProcess::CreateProcessArguments* arguments) { modify(arguments, *state); });
    // started() means the child exists: a refused one is ended there.
    // FailedToStart means it does not: only the warning is written.
    QProcess* const self = &process;
    QObject::connect(
        &process, &QProcess::started, &process,
        [state, self]() {
            if (!state->refused) return;
            tellRefusal(*state);
            endRefused(*self);
        },
        Qt::DirectConnection);
    QObject::connect(
        &process, &QProcess::errorOccurred, &process,
        [state](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && state->refused) tellRefusal(*state);
        },
        Qt::DirectConnection);
#else
    passOnlyStdio(process, {}, {});
#endif
}

#ifndef Q_OS_WIN
void passOnlyStdio(QProcess& process, std::vector<int> leftOpen, std::function<void()> thenInChild)
{
    if (alreadySet(process)) return;
    Plan plan;
    for (const int fd : leftOpen)
        if (fd > 2) plan.leftOpen.push_back(fd);
    std::sort(plan.leftOpen.begin(), plan.leftOpen.end());
    plan.leftOpen.erase(std::unique(plan.leftOpen.begin(), plan.leftOpen.end()),
                        plan.leftOpen.end());
    plan.bound = descriptorBound();
    plan.then = std::move(thenInChild);
    process.setChildProcessModifier([plan = std::move(plan)]() {
        markInChild(plan);
        if (plan.then) plan.then();
    });
}
#endif

namespace childstdio {
void refuseNextForTests()
{
    g_RefuseNext = true;
}
} // namespace childstdio

} // namespace mw
