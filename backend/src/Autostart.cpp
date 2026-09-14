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

#include "Autostart.h"

#include "common/DesktopSession.h"
#include "common/Edition.h"
#include "common/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <comdef.h>
#include <taskschd.h>
#endif

namespace Autostart {

// One rule for every platform: a login item is for a desktop session. Under a
// service supervisor (MW_SERVICE) the server is up before anyone logs in, and a
// login item on top of it would only ever launch a second instance that loses
// the lock — on Windows, that is exactly the tray-client dance a service
// install already goes through, and not something to offer as an option.
bool isSupported()
{
    return mw::hasDesktopSession();
}

namespace {

// A --dev instance keeps its login item apart, the way it keeps its state
// apart (dataName() is productName() + "-dev"): otherwise a switch flipped in
// a --dev run of the production binary would rewrite the real install's login
// item to point at build\MoonlightWeb.exe — and its launch has to carry --dev
// too, or the next logon would start a production instance from that path.
QString devSuffix()
{
    return mw::edition::devFlag() ? QStringLiteral("-dev") : QString();
}

QString launchArguments()
{
    return mw::edition::devFlag() ? QStringLiteral("--autostart --dev")
                                  : QStringLiteral("--autostart");
}

} // namespace

#if defined(Q_OS_MACOS)

namespace {

// The DEV build has its own agent, the one its installer writes (postinstall):
// sharing the label would have each edition overwrite the other's login item.
QString label()
{
    return (mw::edition::isDevBuild() ? QStringLiteral("com.moonlightweb.agent.dev")
                                      : QStringLiteral("com.moonlightweb.agent")) +
           devSuffix();
}

QString plistPath()
{
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/") + label() +
           QStringLiteral(".plist");
}

// Minimal XML text escaping for values embedded in the plist (paths may contain
// '&' etc.). Order matters: '&' first.
QString xmlEscape(QString s)
{
    s.replace('&', QStringLiteral("&amp;"));
    s.replace('<', QStringLiteral("&lt;"));
    s.replace('>', QStringLiteral("&gt;"));
    return s;
}

} // namespace

bool installLoginItem()
{
    const QString exe = QCoreApplication::applicationFilePath();
    const QString logDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    const QString log = logDir + QStringLiteral("/agent.log");

    // No MW_SERVICE here: this is a normal GUI launch (tray icon), mirroring the
    // Windows logon Scheduled Task. KeepAlive/SuccessfulExit=false → relaunch only
    // on a crash, never after a clean quit.
    const QString plist = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                         "<plist version=\"1.0\">\n"
                                         "<dict>\n"
                                         "    <key>Label</key><string>%1</string>\n"
                                         "    <key>ProgramArguments</key>\n"
                                         "    <array><string>%2</string>%4</array>\n"
                                         "    <key>RunAtLoad</key><true/>\n"
                                         "    <key>KeepAlive</key>\n"
                                         "    <dict><key>SuccessfulExit</key><false/></dict>\n"
                                         "    <key>ThrottleInterval</key><integer>5</integer>\n"
                                         // Interactive: without it launchd applies background
                                         // resource limits (timer coalescing/QoS) that stall the
                                         // streaming pipeline. See common/MacActivity.h.
                                         "    <key>ProcessType</key><string>Interactive</string>\n"
                                         "    <key>StandardOutPath</key><string>%3</string>\n"
                                         "    <key>StandardErrorPath</key><string>%3</string>\n"
                                         "</dict>\n"
                                         "</plist>\n")
                              .arg(xmlEscape(label()), xmlEscape(exe), xmlEscape(log),
                                   QStringLiteral("<string>") +
                                       launchArguments()
                                           .split(QLatin1Char(' '))
                                           .join(QStringLiteral("</string><string>")) +
                                       QStringLiteral("</string>"));

    const QString path = plistPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::warning(QStringLiteral("Autostart: cannot write %1").arg(path));
        return false;
    }
    f.write(plist.toUtf8());
    f.close();
    // Do NOT `launchctl bootstrap` here: this instance is already running, and
    // loading the agent now would launch a duplicate. launchd loads the plist at
    // the next login on its own.
    Logger::info(QStringLiteral("Autostart: login item installed at %1").arg(path));
    return true;
}

bool removeLoginItem()
{
    const QString path = plistPath();
    if (!QFile::exists(path)) return true;
    // Not unloaded either: launchd only reads the plist at login, and this
    // instance keeps running until the user quits it — removing the file is
    // the whole of "don't start me next time".
    if (!QFile::remove(path)) {
        Logger::warning(QStringLiteral("Autostart: cannot remove %1").arg(path));
        return false;
    }
    Logger::info(QStringLiteral("Autostart: login item removed from %1").arg(path));
    return true;
}

bool isLoginItemInstalled()
{
    return QFile::exists(plistPath());
}

#elif defined(Q_OS_LINUX)

namespace {

// XDG autostart entry (~/.config/autostart), honored by GNOME/KDE/XFCE and
// every desktop implementing the Desktop Application Autostart spec.
QString entryPath()
{
    // Per edition, for the same reason as the macOS label: two editions must
    // not overwrite each other's login item.
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/autostart/") +
           (mw::edition::isDevBuild() ? QStringLiteral("moonlightweb-dev")
                                      : QStringLiteral("moonlightweb")) +
           devSuffix() + QStringLiteral(".desktop");
}

} // namespace

bool installLoginItem()
{
    // Inside an AppImage, applicationFilePath() points at the transient mount;
    // relaunch the .AppImage itself instead ($APPIMAGE is set by the runtime).
    QString exe = qEnvironmentVariable("APPIMAGE");
    if (exe.isEmpty()) exe = QCoreApplication::applicationFilePath();

    // The icon name is the one the package installed in the hicolor theme.
    const QString entry = QStringLiteral("[Desktop Entry]\n"
                                         "Type=Application\n"
                                         "Name=%2\n"
                                         "Comment=Sunshine streaming client for the browser\n"
                                         "Exec=\"%1\" %4\n"
                                         "Icon=%3\n"
                                         "Terminal=false\n"
                                         "X-GNOME-Autostart-enabled=true\n")
                              .arg(exe, mw::edition::displayName(),
                                   mw::edition::isDevBuild() ? QStringLiteral("moonlightweb-dev")
                                                             : QStringLiteral("moonlightweb"),
                                   launchArguments());

    const QString path = entryPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::warning(QStringLiteral("Autostart: cannot write %1").arg(path));
        return false;
    }
    f.write(entry.toUtf8());
    f.close();
    Logger::info(QStringLiteral("Autostart: login item installed at %1").arg(path));
    return true;
}

bool removeLoginItem()
{
    const QString path = entryPath();
    if (!QFile::exists(path)) return true;
    if (!QFile::remove(path)) {
        Logger::warning(QStringLiteral("Autostart: cannot remove %1").arg(path));
        return false;
    }
    Logger::info(QStringLiteral("Autostart: login item removed from %1").arg(path));
    return true;
}

bool isLoginItemInstalled()
{
    return QFile::exists(entryPath());
}

#else // Windows: the logon Scheduled Task, shared with the Inno Setup installer

namespace {

// The task is looked up, written and deleted through the Task Scheduler COM
// API rather than by spawning schtasks.exe: the tray asks "is it there?" every
// time its menu opens, and a child process per click is the wrong price for
// that. The API also answers with an HRESULT instead of a localized message.

// Task Scheduler wants an apartment; Qt's GUI thread already has one (STA), in
// which case CoInitializeEx says S_FALSE and we still pair it with an
// uninitialize. RPC_E_CHANGED_MODE means some other apartment is already up on
// this thread — usable as it is, nothing to undo.
struct ComScope
{
    HRESULT hr;
    ComScope()
        : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {}
    ~ComScope()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// A BSTR with the lifetime of a scope.
struct Bstr
{
    BSTR b;
    explicit Bstr(const QString& s)
        : b(SysAllocString(reinterpret_cast<const OLECHAR*>(s.utf16())))
    {}
    ~Bstr() { SysFreeString(b); }
};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1 %2")
        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))
        .arg(QString::fromWCharArray(_com_error(hr).ErrorMessage()));
}

// The root folder of the scheduler, where the installer put the task, or null
// with the reason logged.
bool openRootFolder(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& root)
{
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.out()));
    if (FAILED(hr)) {
        Logger::warning(
            QStringLiteral("Autostart: Task Scheduler unavailable: %1").arg(hresultText(hr)));
        return false;
    }
    VARIANT none;
    VariantInit(&none);
    hr = service->Connect(none, none, none, none);
    if (FAILED(hr)) {
        Logger::warning(
            QStringLiteral("Autostart: Task Scheduler connect failed: %1").arg(hresultText(hr)));
        return false;
    }
    Bstr rootPath(QStringLiteral("\\"));
    hr = service->GetFolder(rootPath.b, root.out());
    if (FAILED(hr)) {
        Logger::warning(
            QStringLiteral("Autostart: Task Scheduler root folder: %1").arg(hresultText(hr)));
        return false;
    }
    return true;
}

// Same name as the installer's task, so the box in the app and the box in the
// installer talk about one and the same thing. Per edition, like every other
// registration: the DEV build must not switch the production task on or off —
// and a --dev instance gets its own ("MoonlightWeb-dev"), see devSuffix().
QString taskName()
{
    return mw::edition::productName() + devSuffix();
}

QString xmlEscape(QString s)
{
    s.replace('&', QStringLiteral("&amp;"));
    s.replace('<', QStringLiteral("&lt;"));
    s.replace('>', QStringLiteral("&gt;"));
    s.replace('"', QStringLiteral("&quot;"));
    return s;
}

// DOMAIN\user for the principal, the way the installer writes it.
QString currentUser()
{
    const QString domain = qEnvironmentVariable("USERDOMAIN");
    const QString name = qEnvironmentVariable("USERNAME");
    return domain.isEmpty() ? name : domain + QLatin1Char('\\') + name;
}

} // namespace

bool installLoginItem()
{
    ComScope com;
    if (!com.usable()) return false;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!openRootFolder(service, root)) return false;

    // The definition the installer registers (RegisterLogonTask in
    // moonlightweb.iss), element for element: a logon trigger for this user,
    // run with the interactive token at least privilege — so the task is the
    // user's own and needs no elevation to write, read or drop — relaunched
    // up to three times a minute apart if it crashes, never stopped for
    // running long, and a second launch ignored while one is up.
    const QString user = xmlEscape(currentUser());
    const QString exe =
        xmlEscape(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    const QString xml =
        QStringLiteral(
            "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
            "<RegistrationInfo><Author>%3</Author></RegistrationInfo>"
            "<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%1</UserId></LogonTrigger>"
            "</Triggers>"
            "<Principals><Principal id=\"Author\"><UserId>%1</UserId>"
            "<LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel>"
            "</Principal></Principals>"
            "<Settings>"
            "<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>"
            "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
            "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
            "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
            "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
            "</Settings>"
            "<Actions Context=\"Author\"><Exec><Command>\"%2\"</Command>"
            "<Arguments>%4</Arguments></Exec></Actions>"
            "</Task>")
            .arg(user, exe, xmlEscape(taskName()), launchArguments());

    Bstr name(taskName());
    Bstr definition(xml);
    VARIANT none;
    VariantInit(&none);
    ComPtr<IRegisteredTask> registered;
    const HRESULT hr = root->RegisterTask(name.b, definition.b, TASK_CREATE_OR_UPDATE, none, none,
                                          TASK_LOGON_INTERACTIVE_TOKEN, none, registered.out());
    if (FAILED(hr)) {
        Logger::warning(QStringLiteral("Autostart: registering task \"%1\" failed: %2")
                            .arg(taskName(), hresultText(hr)));
        return false;
    }
    Logger::info(QStringLiteral("Autostart: logon task \"%1\" registered").arg(taskName()));
    return true;
}

bool removeLoginItem()
{
    ComScope com;
    if (!com.usable()) return false;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!openRootFolder(service, root)) return false;

    Bstr name(taskName());
    const HRESULT hr = root->DeleteTask(name.b, 0);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
    if (FAILED(hr)) {
        Logger::warning(QStringLiteral("Autostart: deleting task \"%1\" failed: %2")
                            .arg(taskName(), hresultText(hr)));
        return false;
    }
    Logger::info(QStringLiteral("Autostart: logon task \"%1\" deleted").arg(taskName()));
    return true;
}

bool isLoginItemInstalled()
{
    ComScope com;
    if (!com.usable()) return false;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!openRootFolder(service, root)) return false;

    Bstr name(taskName());
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(root->GetTask(name.b, task.out()));
}

#endif

} // namespace Autostart
