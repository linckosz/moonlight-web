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

#include <functional>

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
    /// @p server may be null in client mode, where the URL provider is the only
    /// source of URLs.
    explicit TrayManager(HttpServer* server, QObject* parent = nullptr);
    ~TrayManager();

    /// Create and show the tray icon. Returns false if the system tray is unavailable.
    bool init();

    /// Tray-only mode: this process runs no server of its own, it decorates a
    /// MoonlightWeb instance that lives where nobody can see it (the Windows
    /// service in session 0, a systemd unit). The menu says so, and its Restart /
    /// Quit entries act on that remote server through the handlers below rather
    /// than on this bare tray. Call before init().
    void setClientMode(bool on) { m_ClientMode = on; }

    /// Client mode: how the menu drives the server it decorates. main.cpp wires
    /// these to a loopback POST (/api/system/restart, /api/system/quit) so the
    /// server exits on its own and its supervisor restarts it (Restart) or lets
    /// it stay down (Quit) — no elevation, works against the session-0 service.
    /// With no handler set, Restart is a no-op and Quit just closes this tray.
    void setRestartHandler(std::function<void()> fn) { m_RestartServer = std::move(fn); }
    void setQuitHandler(std::function<void()> fn) { m_QuitServer = std::move(fn); }

    /// Resolve the application icon from the shipped frontend assets (PNG
    /// preferred: QtGui decodes it without the imageformats .ico plugin).
    /// Also used by main() as the Dock/taskbar icon fallback. May be null.
    static QIcon loadAppIcon();

    /// Override the URL the tray entries open. main.cpp installs a provider that
    /// returns the public domain (with the host key) once Internet Access is
    /// live, and https://localhost otherwise. When unset (or when the provider
    /// returns an empty URL), the built-in localhost URL is used.
    void setUrlProvider(std::function<QUrl(const QString& path)> provider)
    {
        m_UrlProvider = std::move(provider);
    }

    /// Supplies the address this machine answers at from anywhere, or an empty
    /// string when there is none yet. It is deliberately NOT what the tray's
    /// Open entry uses: opening it from the host machine would leave the LAN to
    /// come straight back, and loopback already carries the host-key privilege
    /// that the remote address does not. What it is for is the one gesture the
    /// tray is genuinely the right place for — being at this PC and needing the
    /// address on a phone. When unset, that entry is not shown at all.
    void setRemoteUrlProvider(std::function<QString()> provider)
    {
        m_RemoteUrlProvider = std::move(provider);
    }

    /// Recompute the hover tooltip from the current entry URL. Call after the
    /// HTTPS port or the public domain changed (port parity rebind, Internet
    /// Access becoming ready).
    void refreshTooltip();

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

    HttpServer* m_Server; // null in client mode
    bool m_ClientMode = false;
    std::function<QUrl(const QString& path)> m_UrlProvider;
    std::function<QString()> m_RemoteUrlProvider;
    QAction* m_CopyRemoteAction = nullptr;
    std::function<void()> m_RestartServer; // client mode: restart the remote server
    std::function<void()> m_QuitServer;    // client mode: stop the remote server
    QSystemTrayIcon* m_TrayIcon;
    QMenu* m_Menu;
    QMenu* m_DockMenu;         // macOS Dock right-click menu (null elsewhere)
    QElapsedTimer m_StartedAt; // filters out the app-launch activation (macOS)
};
