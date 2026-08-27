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

#include "TrayManager.h"
#include "server/HttpServer.h"

#include <QApplication>
#include <QStyle>
#include <QDesktopServices>
#include <QUrl>
#include <QIcon>
#include <QFile>
#include <QProcess>
#include <QCoreApplication>
#include <QAction>
#include <QGuiApplication>
#include <QMenu>

TrayManager::TrayManager(HttpServer* server, QObject* parent)
    : QObject(parent)
    , m_Server(server)
    , m_TrayIcon(nullptr)
    , m_Menu(nullptr)
    , m_DockMenu(nullptr)
{}

QIcon TrayManager::loadAppIcon()
{
    // Try the compile-time frontend path (development), then the
    // executable-relative bundle paths (installed artifact / macOS bundle).
    // PNG before .ico: QtGui decodes PNG natively, while .ico needs the
    // imageformats plugin which is not always deployed on Linux.
    const QStringList roots = {
        QStringLiteral(FRONTEND_DIR),
        QCoreApplication::applicationDirPath() + QStringLiteral("/frontend/"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/frontend/"),
    };
    const QStringList names = {QStringLiteral("assets/icon-512.png"),
                               QStringLiteral("assets/favicon.ico")};
    for (const QString& root : roots) {
        for (const QString& name : names) {
            const QString path = root + name;
            if (!QFile::exists(path)) continue;
            QIcon icon(path);
            if (!icon.isNull()) return icon;
        }
    }
    return QIcon();
}

TrayManager::~TrayManager()
{
    if (m_TrayIcon) m_TrayIcon->hide();
    delete m_Menu;
    delete m_DockMenu;
}

bool TrayManager::init()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qInfo() << "[TrayManager] System tray not available (headless or no desktop)";
        return false;
    }

    m_TrayIcon = new QSystemTrayIcon(this);
    m_Menu = new QMenu();

    QIcon icon = loadAppIcon();
    if (icon.isNull()) {
        qInfo() << "[TrayManager] app icon not found, using standard icon";
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    m_TrayIcon->setIcon(icon);

    refreshTooltip();

    // Build context menu. In client mode the server is a service in another
    // session this process cannot see: the header says so, and Restart / Quit act
    // on that server (over loopback, via the handlers) rather than on this bare
    // tray — so they are named "… Server" to say whose lifetime they touch.
    if (m_ClientMode) {
        QAction* header = m_Menu->addAction(tr("Server runs as a system service"));
        header->setEnabled(false);
        m_Menu->addSeparator();
    }
    QAction* openAction = m_Menu->addAction(tr("&Open"));
    QAction* controlPanelAction = m_Menu->addAction(tr("&Server Settings"));
    m_Menu->addSeparator();
    QAction* restartAction =
        m_Menu->addAction(m_ClientMode ? tr("&Restart Server") : tr("&Restart"));
    m_Menu->addSeparator();
    QAction* quitAction = m_Menu->addAction(m_ClientMode ? tr("&Quit Server") : tr("&Quit"));

    connect(openAction, &QAction::triggered, this, &TrayManager::onOpen);
    connect(controlPanelAction, &QAction::triggered, this, &TrayManager::onOpenSettings);
    connect(restartAction, &QAction::triggered, this, &TrayManager::onRestart);
    connect(quitAction, &QAction::triggered, this, &TrayManager::onQuit);
    connect(m_TrayIcon, &QSystemTrayIcon::activated, this, &TrayManager::onActivated);

    m_TrayIcon->setContextMenu(m_Menu);
    m_TrayIcon->show();

#ifdef Q_OS_MACOS
    // Dock right-click menu — reuse the tray actions; macOS appends its own
    // Quit entry, so ours is omitted here.
    m_DockMenu = new QMenu();
    m_DockMenu->addAction(openAction);
    m_DockMenu->addAction(controlPanelAction);
    m_DockMenu->addSeparator();
    m_DockMenu->addAction(restartAction);
    m_DockMenu->setAsDockMenu();

    // Clicking the Dock icon re-activates the app; with no native window the
    // expected result is the admin page in the browser. The app also activates
    // once at launch — filter that out with a short startup grace window.
    m_StartedAt.start();
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive && m_StartedAt.elapsed() > 3000)
                    onOpenSettings();
            });
#endif

    // Query stripped: the entry URL carries the single-use host key (?mwk=...),
    // which has no business in a log file.
    QUrl logged = localUrl(QString());
    logged.setQuery(QString());
    qInfo() << "[TrayManager] System tray icon created" << (m_ClientMode ? "(client mode)" : "")
            << "for" << logged.toString();
    return true;
}

void TrayManager::refreshTooltip()
{
    if (!m_TrayIcon) return;
    QUrl url = localUrl(QString());
    // Never expose the host key (?mwk=...) in the hover tooltip.
    url.setQuery(QString());
    m_TrayIcon->setToolTip(QStringLiteral("MoonlightWeb\n") + url.toString());
}

void TrayManager::onActivated(QSystemTrayIcon::ActivationReason reason)
{
    // Double-click opens the browser (Windows convention for tray default action)
    if (reason == QSystemTrayIcon::DoubleClick) onOpen();
}

// Build a localhost URL, preferring HTTPS; fall back to plain HTTP when the
// TLS listener is down (e.g. cert generation failed) so the tray still works.
// A provider installed by main.cpp takes precedence (public domain + host key
// once Internet Access is live).
QUrl TrayManager::localUrl(const QString& path) const
{
    if (m_UrlProvider) {
        QUrl url = m_UrlProvider(path);
        if (!url.isEmpty()) return url;
    }

    if (!m_Server) return QUrl(); // client mode: the provider above is all there is

    quint16 httpsPort = m_Server->activeHttpsPort();
    if (httpsPort != 0)
        return QUrl(QStringLiteral("https://localhost:%1%2").arg(httpsPort).arg(path));

    quint16 httpPort = m_Server->httpPort();
    if (httpPort != 0) return QUrl(QStringLiteral("http://localhost:%1%2").arg(httpPort).arg(path));

    return QUrl();
}

void TrayManager::onOpen()
{
    QUrl url = localUrl(QString());
    if (url.isEmpty()) {
        qWarning() << "[TrayManager] Cannot open — no HTTP/HTTPS listener running";
        return;
    }
    qInfo() << "[TrayManager] Opening" << url.toString();
    QDesktopServices::openUrl(url);
}

// One entry, two possible addresses, and the choice is not the user's to make.
//
// The internet link is preferred whenever there is one, for a single reason: it
// is the only address that reaches this page under a certificate the browser
// already trusts. Loopback never will be — it is served by the self-signed
// certificate, and every visit costs an acceptance or a scary interstitial.
//
// The provider answers empty whenever that link would not work: the option is
// off, the line is down, or the introduction server did not respond when asked.
// So the fallback is not a guess — by the time we reach it, the internet has
// already been tried and found wanting, and loopback is what is left that
// always works. Nothing is said about it either way; the owner asked for the
// settings page, not for a report on how it was reached.
void TrayManager::onOpenSettings()
{
    const QString link = m_RemoteAdminLink ? m_RemoteAdminLink() : QString();
    if (!link.isEmpty()) {
        // Logged without its fragment. The key in there is single-use, but it is
        // still a credential, and a log file is exactly the place it must not
        // outlive its one use in.
        QUrl logged(link);
        logged.setFragment(QString());
        qInfo() << "[TrayManager] Opening settings over the internet link" << logged.toString();
        QDesktopServices::openUrl(QUrl(link));
        return;
    }

    QUrl url = localUrl(QStringLiteral("/admin"));
    if (url.isEmpty()) {
        qWarning() << "[TrayManager] Cannot open settings — no HTTP/HTTPS listener running";
        return;
    }
    qInfo() << "[TrayManager] Opening settings" << url.toString();
    QDesktopServices::openUrl(url);
}

void TrayManager::onRestart()
{
    // Client mode: the "server" is a service in another session. Ask it to
    // restart over loopback (its supervisor respawns it) and keep this tray alive
    // to redecorate the new instance — the port poll in runTrayClient follows it.
    if (m_ClientMode) {
        if (!m_RestartServer) {
            qWarning() << "[TrayManager] Restart requested but no server handler is set";
            return;
        }
        qInfo() << "[TrayManager] Asking the server to restart...";
        if (m_TrayIcon)
            m_TrayIcon->showMessage(QStringLiteral("MoonlightWeb"), tr("Restarting the server…"),
                                    QSystemTrayIcon::Information, 3000);
        m_RestartServer();
        return;
    }

    qInfo() << "[TrayManager] Restarting application...";

    // Launch a new instance with the same arguments, then quit this one
    QString appPath = QCoreApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) args.removeFirst(); // argv[0] is the app path, startDetached handles it

    if (!QProcess::startDetached(appPath, args)) {
        qWarning() << "[TrayManager] Restart failed — could not launch new process";
        return;
    }

    qInfo() << "[TrayManager] New instance launched, quitting this one";
    QApplication::quit();
}

void TrayManager::onQuit()
{
    // Client mode: "Quit Server" stops the service over loopback, then this tray
    // (which only decorated it) closes too. With no handler wired, fall back to
    // just closing the tray — the old non-destructive "Hide Tray Icon" behavior.
    if (m_ClientMode && m_QuitServer) {
        qInfo() << "[TrayManager] Asking the server to stop...";
        m_QuitServer();
    } else {
        qInfo() << "[TrayManager] Quitting on user request";
    }
    QApplication::quit();
}
