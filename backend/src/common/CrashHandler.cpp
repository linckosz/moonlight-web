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

#include "CrashHandler.h"
#include "Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

namespace CrashHandler {

// Keep only the most recent N dumps so a crash loop cannot fill the disk.
static constexpr int kMaxDumps = 10;

#ifdef Q_OS_WIN

// Directory for dumps, resolved at install() time and kept as a NUL-terminated
// wide string. The exception filter runs in a corrupted process, so it must not
// touch the heap (no QString/std::string): everything below uses stack buffers
// and plain Win32 calls only.
static wchar_t g_CrashDir[MAX_PATH] = {0};

// Runs when no other handler caught the exception. Writes a minidump, then
// returns EXCEPTION_CONTINUE_SEARCH so the OS default handler still terminates
// the process with a non-zero code (the service supervisor then restarts it).
static LONG WINAPI writeDump(EXCEPTION_POINTERS* ex)
{
    if (g_CrashDir[0] == L'\0') return EXCEPTION_CONTINUE_SEARCH;

    // Build "<dir>\crash-YYYYMMDD-HHMMSS-<pid>.dmp" with stack buffers only.
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_CrashDir, st.wYear,
              st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              static_cast<unsigned long>(GetCurrentProcessId()));

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ex;
        mei.ClientPointers = FALSE;

        // MiniDumpNormal = thread stacks + loaded module list. Enough to resolve
        // the crash call stack with the .pdb, while staying small (a few hundred
        // KB) and not dumping the full heap (no bulk secrets on disk).
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                          ex ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#endif // Q_OS_WIN

// Delete all but the newest kMaxDumps *.dmp files. Runs at startup (normal
// process state), never from the crashing filter.
static void pruneOldDumps(const QString& crashDir)
{
    QDir dir(crashDir);
    QFileInfoList dumps = dir.entryInfoList({QStringLiteral("*.dmp")}, QDir::Files, QDir::Time);
    for (int i = kMaxDumps; i < dumps.size(); ++i) {
        QFile::remove(dumps[i].absoluteFilePath());
    }
}

void install(const QString& crashDir)
{
    QDir().mkpath(crashDir);
    pruneOldDumps(crashDir);

#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(crashDir);
    // Leave g_CrashDir empty (handler becomes a no-op) if the path is too long
    // to hold a dump filename safely.
    if (native.size() < MAX_PATH - 64) {
        native.toWCharArray(g_CrashDir);
        g_CrashDir[native.size()] = L'\0';
        SetUnhandledExceptionFilter(writeDump);
        Logger::info(QString("[CrashHandler] Minidump handler installed — dumps in %1").arg(native));
    } else {
        Logger::warning("[CrashHandler] Crash dir path too long — minidump handler not installed");
    }
#else
    Logger::info("[CrashHandler] Native crash dumps rely on the OS core-dump facility");
#endif
}

} // namespace CrashHandler
