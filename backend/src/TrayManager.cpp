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

    // Build context menu
    QAction* openAction = m_Menu->addAction(tr("&Open"));
    QAction* controlPanelAction = m_Menu->addAction(tr("&Server Settings"));
    m_Menu->addSeparator();
    QAction* restartAction = m_Menu->addAction(tr("&Restart"));
    m_Menu->addSeparator();
    QAction* quitAction = m_Menu->addAction(tr("&Quit"));

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

    qInfo() << "[TrayManager] System tray icon created, port" << m_Server->activeHttpsPort();
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

void TrayManager::onOpenSettings()
{
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
    qInfo() << "[TrayManager] Quitting on user request";
    QApplication::quit();
}
