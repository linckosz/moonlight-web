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
#include <QSet>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QTimer>
#include <QUrl>
#include <QIcon>
#include <QElapsedTimer>

#include "ViewerNotices.h"
#include "server/StreamActivity.h"

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

    /// Booth mode (--noadmin): the menu keeps Open and Streaming only — no
    /// Server Settings, Start at login, Restart or Quit. Call before init().
    void setAdminLocked(bool on) { m_AdminLocked = on; }

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

    /// Who is streaming this screen, asked every @p intervalMs.
    ///
    /// Polled rather than pushed because the tray is usually not in the process
    /// that knows: under a service the icon belongs to a client that only has
    /// loopback (see runTrayClient). The server-side tray answers from its own
    /// slot table instead, and both then take the same three consequences —
    /// the tooltip, the "Streaming (n)" entry, and a notification per arrival
    /// and per departure.
    ///
    /// Call before init(). With no provider the tray never mentions streaming.
    void setActivityProvider(std::function<StreamActivity()> provider, int intervalMs = 2000)
    {
        m_Activity = std::move(provider);
        m_ActivityIntervalMs = intervalMs;
    }

private slots:
    void onActivated(QSystemTrayIcon::ActivationReason reason);
    void onOpen();
    void onOpenSettings();
    void onOpenSessions();
    void onRestart();
    void onQuit();
    void onAutostartToggled(bool on);

private:
    /// Re-read the login item from the OS before the menu shows, so the tick
    /// says what is true right now (the admin page or the user's own hands may
    /// have changed it since the last look).
    void refreshAutostart();
    /// Ask the provider, then say what changed: tooltip, menu entries, the red
    /// dot, and one notification per viewer who arrived or left.
    void pollActivity();
    /// The next waiting notice out, if it is due and Windows would show it now.
    void releaseNotice();
    /// "MoonlightWeb", plus "Streaming (n)" on a second line while anyone is.
    void refreshTooltip(int count);
    /// The "Recent viewers" submenu, rebuilt as the menu opens.
    void refreshRecentMenu();
    /// A notification under the product's name, with the information icon.
    void notify(const QString& text);
    /// That icon, rendered once per device pixel ratio (Windows, see the .cpp).
    QIcon noticeIcon();
    /// @p base with a red dot in its corner, at the sizes a tray asks for.
    static QIcon withDot(const QIcon& base);
    /// localhost URL for `path`, preferring HTTPS, falling back to HTTP; empty if
    /// no listener is up.
    QUrl localUrl(const QString& path) const;

    /// Open `path` over the internet link if there is one, over loopback
    /// otherwise. Both menu entries that lead into the app go through here.
    void openAppPage(const QString& path);

    HttpServer* m_Server; // null in client mode
    bool m_ClientMode = false;
    bool m_AdminLocked = false;
    std::function<QUrl(const QString& path)> m_UrlProvider;
    std::function<QString(const QString& path)> m_RemoteLink; // see setRemoteLinkProvider
    std::function<void()> m_RestartServer; // client mode: restart the remote server
    std::function<void()> m_QuitServer;    // client mode: stop the remote server
    QSystemTrayIcon* m_TrayIcon;
    QMenu* m_Menu;
    QMenu* m_DockMenu;                    // macOS Dock right-click menu (null elsewhere)
    QAction* m_AutostartAction = nullptr; // "Start at login" — only where it can be done
    QElapsedTimer m_StartedAt;            // filters out the app-launch activation (macOS)
    QIcon m_NoticeIcon;                   // a notification's icon, rendered once...
    qreal m_NoticeIconDpr = 0;            // ...at this device pixel ratio

    // ── Streaming activity ─────────────────────────────────────────────────
    std::function<StreamActivity()> m_Activity;
    int m_ActivityIntervalMs = 2000;
    QTimer m_ActivityTimer;
    QAction* m_StreamingAction = nullptr;    // hidden while nobody is streaming
    QMenu* m_RecentMenu = nullptr;           // "Recent viewers", hidden while empty
    QAction* m_StreamingSeparator = nullptr; // hidden with them
    // Who arrived and left, the notices waiting for Windows, and the recent
    // viewers: in this process's memory only (see ViewerNotices).
    ViewerNotices m_Notices;
    QElapsedTimer m_Clock; // the gap between two notices
    QIcon m_Icon;          // the icon, and the same with the red dot,
    QIcon m_IconWatched;   // both rendered at init, never during a stream
    bool m_ShowsDot = false;
};
