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

#include <QAction>
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

    /// `path` over the internet link — empty for the front door, "/admin" for
    /// the settings page — or an empty string when there is no such link.
    ///
    /// This is what Open and Server Settings use when they can, because it is
    /// the only door that opens under a certificate the browser already trusts —
    /// loopback is served by a self-signed certificate and always will be. The
    /// link carries the single-use host key in its FRAGMENT, never a query, so
    /// the introduction server it is fetched from never sees it.
    ///
    /// Each is one entry, not two. A second menu item would have asked the owner
    /// to know which of two addresses reaches their own machine, which is not
    /// something they should have to hold an opinion about. Empty means "the
    /// internet is not an option here, or not one that answers right now", and
    /// the menu falls back to loopback without saying anything.
    ///
    /// Asked for at the moment it is clicked rather than cached, because every
    /// part of the answer moves: the option can be switched off, the line goes
    /// up and down with the network, and the key is burnt and rotated the
    /// instant it is redeemed. That is also why only the menu gets this and the
    /// tooltip and the desktop shortcut do not: they are written down ahead of
    /// the click, and a single-use key cannot be.
    void setRemoteLinkProvider(std::function<QString(const QString& path)> provider)
    {
        m_RemoteLink = std::move(provider);
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

    /// Open `path` over the internet link if there is one, over loopback
    /// otherwise. Both menu entries that lead into the app go through here.
    void openAppPage(const QString& path);

    HttpServer* m_Server; // null in client mode
    bool m_ClientMode = false;
    std::function<QUrl(const QString& path)> m_UrlProvider;
    std::function<QString(const QString& path)> m_RemoteLink; // see setRemoteLinkProvider
    std::function<void()> m_RestartServer; // client mode: restart the remote server
    std::function<void()> m_QuitServer;    // client mode: stop the remote server
    QSystemTrayIcon* m_TrayIcon;
    QMenu* m_Menu;
    QMenu* m_DockMenu;         // macOS Dock right-click menu (null elsewhere)
    QElapsedTimer m_StartedAt; // filters out the app-launch activation (macOS)
};
