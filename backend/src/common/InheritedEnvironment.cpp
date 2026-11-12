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

#include "common/InheritedEnvironment.h"

#include <QStringList>
#include <QtGlobal>

#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace mw::inheritedenv {

bool isCodeLoading(const QString& name)
{
    // Exact names: Qt's plugin and QML search paths (a platform, TLS or image
    // plugin is a DLL Qt loads by itself), and the files OpenSSL and its users
    // read their trust anchors from.
    static const char* const kExact[] = {
        "QT_PLUGIN_PATH",  "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
        "SSL_CERT_FILE",   "SSL_CERT_DIR",
    };
    for (const char* n : kExact)
        if (name.compare(QLatin1String(n), Qt::CaseInsensitive) == 0) return true;
    // Whole families: OPENSSL_CONF can load an engine or a provider DLL, and
    // OPENSSL_MODULES / OPENSSL_ENGINES say where from; the Vulkan loader's
    // VK_ICD_FILENAMES, VK_DRIVER_FILES and VK_*LAYER* name DLLs to load.
    for (const char* prefix : {"OPENSSL_", "VK_"})
        if (name.startsWith(QLatin1String(prefix), Qt::CaseInsensitive)) return true;
    return false;
}

bool isElevatedHelper(int argc, char** argv)
{
    // The modes an installer task (RunLevel=HighestAvailable), the installer
    // itself or the SYSTEM launcher service start elevated: the update and the
    // two driver helpers, and the stream worker that injects input. Both
    // spellings, "--vhid-apply install" and "--vhid-apply=install".
    static const char* const kModes[] = {"--update-apply", "--vdisplay-apply", "--vhid-apply",
                                         "--stream-worker"};
    for (int i = 1; i < argc; ++i) {
        for (const char* mode : kModes) {
            const std::size_t n = std::strlen(mode);
            if (std::strncmp(argv[i], mode, n) == 0 && (argv[i][n] == '\0' || argv[i][n] == '='))
                return true;
        }
    }
    return false;
}

bool startedElevated()
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool read =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return read && elevation.TokenIsElevated != 0;
#else
    return false;
#endif
}

void scrub()
{
#ifdef Q_OS_WIN
    // Names first, removal second: unsetting while walking the block would
    // invalidate it.
    QStringList doomed;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            const QString entry = QString::fromWCharArray(p);
            // "=C:=C:\\..." entries (per-drive current directories) start with
            // '=': the name is never empty, so look for the separator after it.
            const qsizetype eq = entry.indexOf(QLatin1Char('='), 1);
            if (eq > 0 && isCodeLoading(entry.left(eq))) doomed << entry.left(eq);
        }
        FreeEnvironmentStringsW(block);
    }
    // qunsetenv clears the C runtime's copy too, which is the one Qt reads.
    for (const QString& name : doomed)
        qunsetenv(name.toLocal8Bit().constData());
    // The current directory out of the DLL search order: a task starts in
    // System32, a hand-started helper wherever the user stood.
    SetDllDirectoryW(L"");
#endif
}

} // namespace mw::inheritedenv
