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

#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QUrl>
#include <QIcon>
#include <QElapsedTimer>

class HttpServer;

class TrayManager : public QObject
{
    Q_OBJECT

public:
    explicit TrayManager(HttpServer* server, QObject* parent = nullptr);
    ~TrayManager();

    /// Create and show the tray icon. Returns false if the system tray is unavailable.
    bool init();

    /// Resolve the application icon from the shipped frontend assets (PNG
    /// preferred: QtGui decodes it without the imageformats .ico plugin).
    /// Also used by main() as the Dock/taskbar icon fallback. May be null.
    static QIcon loadAppIcon();

private slots:
    void onActivated(QSystemTrayIcon::ActivationReason reason);
    void onOpen();
    void onOpenSettings();
    void onRestart();
    void onQuit();

private:
    /// localhost URL for `path`, preferring HTTPS, falling back to HTTP; empty if
    /// no listener is up.
    QUrl localUrl(const QString& path) const;

    HttpServer* m_Server;
    QSystemTrayIcon* m_TrayIcon;
    QMenu* m_Menu;
    QMenu* m_DockMenu;         // macOS Dock right-click menu (null elsewhere)
    QElapsedTimer m_StartedAt; // filters out the app-launch activation (macOS)
};
