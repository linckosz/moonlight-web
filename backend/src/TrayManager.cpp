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
{}

TrayManager::~TrayManager()
{
    if (m_TrayIcon) m_TrayIcon->hide();
}

bool TrayManager::init()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qInfo() << "[TrayManager] System tray not available (headless or no desktop)";
        return false;
    }

    m_TrayIcon = new QSystemTrayIcon(this);
    m_Menu = new QMenu();

    // Load tray icon — try the compile-time frontend path (development), then the
    // executable-relative bundle path (artifact/MSI), then a standard fallback.
    QString iconPath = QStringLiteral(FRONTEND_DIR "assets/favicon.ico");
    if (!QFile::exists(iconPath))
        iconPath = QCoreApplication::applicationDirPath() + "/frontend/assets/favicon.ico";
    QIcon icon(iconPath);
    if (icon.isNull()) {
        qInfo() << "[TrayManager] favicon.ico not found, using standard icon";
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    m_TrayIcon->setIcon(icon);

    quint16 port = m_Server->activeHttpsPort();
    m_TrayIcon->setToolTip(QStringLiteral("MoonlightWeb\nhttps://localhost:%1").arg(port));

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

    qInfo() << "[TrayManager] System tray icon created, port" << port;
    return true;
}

void TrayManager::onActivated(QSystemTrayIcon::ActivationReason reason)
{
    // Double-click opens the browser (Windows convention for tray default action)
    if (reason == QSystemTrayIcon::DoubleClick) onOpen();
}

// Build a localhost URL, preferring HTTPS; fall back to plain HTTP when the
// TLS listener is down (e.g. cert generation failed) so the tray still works.
QUrl TrayManager::localUrl(const QString& path) const
{
    quint16 httpsPort = m_Server->activeHttpsPort();
    if (httpsPort != 0)
        return QUrl(QStringLiteral("https://localhost:%1%2").arg(httpsPort).arg(path));

    quint16 httpPort = m_Server->httpPort();
    if (httpPort != 0)
        return QUrl(QStringLiteral("http://localhost:%1%2").arg(httpPort).arg(path));

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
