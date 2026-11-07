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

#include "HostAppLauncher.h"

#include "common/Logger.h"
#include "common/DetachedProcess.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#ifdef Q_OS_WIN
#include "streaming/ConsoleSession.h"

#include <windows.h>

#include <exdisp.h>
#include <servprov.h>
#include <shldisp.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <thread>
#endif

namespace HostAppLauncher {

#ifdef Q_OS_WIN

namespace {

using Microsoft::WRL::ComPtr;

struct Variant
{
    VARIANT v;
    Variant() { ::VariantInit(&v); }
    explicit Variant(const QString& s)
    {
        ::VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = ::SysAllocString(reinterpret_cast<const wchar_t*>(s.utf16()));
    }
    explicit Variant(long n)
    {
        ::VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = n;
    }
    ~Variant() { ::VariantClear(&v); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
};

/// Explorer's own ShellExecute, reached through the desktop window's folder
/// view: the program starts as Explorer's child, with Explorer's token.
bool explorerShellExecute(const QString& file, const QString& arguments, QString* error)
{
    const auto fail = [error](const char* step, HRESULT hr) {
        if (error)
            *error = QStringLiteral("%1 failed (0x%2)")
                         .arg(QLatin1String(step))
                         .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        return false;
    };
    ComPtr<IShellWindows> windows;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(&windows));
    if (FAILED(hr)) return fail("CoCreateInstance(ShellWindows)", hr);
    Variant location(static_cast<long>(CSIDL_DESKTOP));
    Variant empty;
    long hwnd = 0;
    ComPtr<IDispatch> desktop;
    hr = windows->FindWindowSW(&location.v, &empty.v, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH,
                               &desktop);
    if (hr != S_OK || !desktop) return fail("FindWindowSW (no Explorer desktop)", hr);
    ComPtr<IServiceProvider> provider;
    hr = desktop.As(&provider);
    if (FAILED(hr)) return fail("IServiceProvider", hr);
    ComPtr<IShellBrowser> browser;
    hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (FAILED(hr)) return fail("QueryService(TopLevelBrowser)", hr);
    ComPtr<IShellView> view;
    hr = browser->QueryActiveShellView(&view);
    if (FAILED(hr)) return fail("QueryActiveShellView", hr);
    ComPtr<IDispatch> background;
    hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background));
    if (FAILED(hr)) return fail("GetItemObject", hr);
    ComPtr<IShellFolderViewDual> folderView;
    hr = background.As(&folderView);
    if (FAILED(hr)) return fail("IShellFolderViewDual", hr);
    ComPtr<IDispatch> application;
    hr = folderView->get_Application(&application);
    if (FAILED(hr)) return fail("get_Application", hr);
    ComPtr<IShellDispatch2> shell;
    hr = application.As(&shell);
    if (FAILED(hr)) return fail("IShellDispatch2", hr);

    BSTR fileB = ::SysAllocString(reinterpret_cast<const wchar_t*>(file.utf16()));
    Variant args(arguments);
    Variant dir(QString{});
    Variant operation(QStringLiteral("open"));
    Variant show(static_cast<long>(SW_SHOWNORMAL));
    hr = shell->ShellExecute(fileB, args.v, dir.v, operation.v, show.v);
    ::SysFreeString(fileB);
    if (FAILED(hr)) return fail("ShellExecute", hr);
    return true;
}

bool processElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

/// open() on this desktop, on the calling thread (COM is set up here).
bool openHere(const QString& command, QString* error)
{
    QString file, arguments;
    if (isUrl(command))
        file = command.trimmed();
    else
        splitWindowsCommand(command, &file, &arguments);
    if (file.isEmpty()) {
        if (error) *error = QStringLiteral("the command names no program");
        return false;
    }
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QString why;
    bool ok = explorerShellExecute(file, arguments, &why);
    if (!ok && !processElevated()) {
        // No Explorer desktop (another shell): this process IS the user at
        // their ordinary level, so its own ShellExecute gives the same.
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = reinterpret_cast<LPCWSTR>(file.utf16());
        info.lpParameters =
            arguments.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(arguments.utf16());
        info.nShow = SW_SHOWNORMAL;
        ok = ::ShellExecuteExW(&info) != FALSE;
        if (!ok) why += QStringLiteral("; ShellExecuteEx error %1").arg(::GetLastError());
    } else if (!ok) {
        // Elevated with no Explorer to hand it to: the program would inherit
        // an administrator's token. Refused.
        why += QStringLiteral("; not opened from an elevated process");
    }
    if (SUCCEEDED(init)) ::CoUninitialize();
    if (!ok && error) *error = why;
    return ok;
}

} // namespace

bool open(const QString& command, QString* error)
{
    if (ConsoleSession::launchesElsewhere()) {
        // The service of older installs, in session 0: this executable again,
        // on the console desktop, as the user logged on there.
        const QString encoded = QString::fromLatin1(command.toUtf8().toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        return ConsoleSession::startAsConsoleUser(QCoreApplication::applicationFilePath(),
                                                  {QStringLiteral("--open-on-desktop"), encoded},
                                                  error);
    }
    // Its own thread: COM wants an apartment of its own, and a busy Explorer
    // must not hold the server's event loop.
    std::thread([command]() {
        QString why;
        if (openHere(command, &why))
            Logger::info(QStringLiteral("[HostApp] opened: %1").arg(command));
        else
            Logger::warning(QStringLiteral("[HostApp] could not open %1: %2").arg(command, why));
    }).detach();
    Q_UNUSED(error);
    return true;
}

int runOpenOnDesktop(const QString& encoded)
{
    const QString command = QString::fromUtf8(QByteArray::fromBase64(
        encoded.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    if (command.isEmpty()) return 2;
    QString why;
    if (openHere(command, &why)) return 0;
    Logger::warning(QStringLiteral("[HostApp] could not open %1: %2").arg(command, why));
    return 1;
}

#else // macOS, Linux

bool open(const QString& command, QString* error)
{
    const QString line = command.trimmed();
    QString program;
    QStringList arguments;
#if defined(Q_OS_MACOS)
    if (isUrl(line) || line.endsWith(QStringLiteral(".app"))) {
        program = QStringLiteral("/usr/bin/open");
        arguments << line;
    } else {
        program = QStringLiteral("/bin/sh");
        arguments << QStringLiteral("-c") << line;
    }
#else
    QStringList run;
    if (isUrl(line))
        run << QStringLiteral("xdg-open") << line;
    else
        run << QStringLiteral("/bin/sh") << QStringLiteral("-c") << line;
    const QString systemdRun = QStandardPaths::findExecutable(QStringLiteral("systemd-run"));
    if (!systemdRun.isEmpty()) {
        // A transient unit of the user's: the program leaves the server's
        // cgroup, so a restart of the server does not take it along. The
        // user manager may not know the desktop's sockets: they are passed,
        // and so are the data dirs, where xdg-open finds a Flatpak's handler.
        QStringList environment;
        for (const char* name :
             {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
              "XDG_CURRENT_DESKTOP", "XDG_DATA_DIRS", "XAUTHORITY"}) {
            const QString value = qEnvironmentVariable(name);
            if (!value.isEmpty())
                environment << QStringLiteral("%1=%2").arg(QLatin1String(name), value);
        }
        program = systemdRun;
        arguments =
            systemdRunArguments(QStringLiteral("moonlightweb-app-%1")
                                    .arg(QUuid::createUuid().toString(QUuid::Id128).left(12)),
                                environment, run);
    } else {
        program = run.takeFirst();
        arguments = run;
    }
#endif
    if (!mw::startDetached(program, arguments)) {
        if (error) *error = QStringLiteral("%1 could not be started").arg(program);
        Logger::warning(QStringLiteral("[HostApp] could not open %1").arg(line));
        return false;
    }
    Logger::info(QStringLiteral("[HostApp] opened: %1").arg(line));
    return true;
}

#endif

#if !defined(Q_OS_MACOS) && !defined(MW_HOST_APP_X11)
// macOS: HostAppLauncherMac.mm. Linux with X11's headers: HostAppLauncherX11.cpp.
bool openOnVirtualDisplay(const QString& command, QString* error)
{
    return open(command, error);
}
#endif

} // namespace HostAppLauncher
