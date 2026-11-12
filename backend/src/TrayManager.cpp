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
#include "Autostart.h"
#include "common/Edition.h"
#include "common/DetachedProcess.h"
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
#include <QPainter>
#include <QSignalBlocker>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

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
    // A DEV identity wears the blue variant, so a DEV install or a --dev
    // instance cannot be taken for the production one in the tray; the
    // production icon stays as a last resort rather than no icon at all.
    const QStringList roots = {
        QStringLiteral(FRONTEND_DIR),
        QCoreApplication::applicationDirPath() + QStringLiteral("/frontend/"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/frontend/"),
    };
    const QStringList names = {mw::edition::iconAsset(QStringLiteral("icon-512.png")),
                               mw::edition::iconAsset(QStringLiteral("favicon.ico")),
                               QStringLiteral("assets/icon-512.png"),
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
    m_Icon = icon;
    m_IconWatched = withDot(icon);
    m_TrayIcon->setIcon(m_Icon);
    m_Clock.start();

    // The name, and — only while it is true — who is watching this screen. A
    // hover tooltip is read at a glance, by someone who is already at this
    // machine and about to click the menu that knows the right address;
    // spelling out a port there was a third place claiming to know the way in,
    // and the one least able to keep up with it. "Streaming (2)" is a different
    // kind of fact: it is about this machine right now, and there is nowhere
    // else it could be read without opening a page.
    refreshTooltip(0);

    // Build context menu. In client mode the server is a service in another
    // session this process cannot see: the header says so, and Restart / Quit act
    // on that server (over loopback, via the handlers) rather than on this bare
    // tray — so they are named "… Server" to say whose lifetime they touch.
    if (m_ClientMode) {
        QAction* header = m_Menu->addAction(tr("Server runs as a system service"));
        header->setEnabled(false);
        m_Menu->addSeparator();
    }
    // Who is watching, above everything else, and only when someone is: an
    // entry that says "Streaming (2)" is news, and it leads to the one page
    // that can say more about it. It stays hidden the rest of the time rather
    // than showing "Streaming (0)" — a permanent entry reading zero is noise,
    // and the tooltip already answers "is anyone?" without a click.
    m_StreamingAction = m_Menu->addAction(tr("Streaming"));
    m_StreamingAction->setVisible(false);
    // Who watched lately, and when: what a notice that went unseen would have
    // said (A137). In memory only, gone with this process; hidden while empty.
    m_RecentMenu = m_Menu->addMenu(tr("Recent viewers"));
    m_RecentMenu->menuAction()->setVisible(false);
    connect(m_Menu, &QMenu::aboutToShow, this, &TrayManager::refreshRecentMenu);
    m_StreamingSeparator = m_Menu->addSeparator();
    m_StreamingSeparator->setVisible(false);
    connect(m_StreamingAction, &QAction::triggered, this, &TrayManager::onOpenSessions);

    QAction* openAction = m_Menu->addAction(tr("&Open"));
    QAction* controlPanelAction = m_Menu->addAction(tr("&Server Settings"));
    m_Menu->addSeparator();
    // Start at login: a tick that says whether the OS will launch this app at
    // the next logon, and flips it. Only where that is ours to do — a desktop
    // session with a mechanism behind it. Not in client mode: the server is a
    // service there, already up before anyone logs in, and a login item beside
    // it would only be another way into the very update trouble a service
    // install has. The tick is re-read from the OS each time the menu opens.
    if (!m_ClientMode && !m_AdminLocked && Autostart::isSupported()) {
        m_AutostartAction = m_Menu->addAction(tr("Start at &login"));
        m_AutostartAction->setCheckable(true);
        connect(m_AutostartAction, &QAction::toggled, this, &TrayManager::onAutostartToggled);
        connect(m_Menu, &QMenu::aboutToShow, this, &TrayManager::refreshAutostart);
        refreshAutostart();
    }
    QAction* restartAction =
        m_Menu->addAction(m_ClientMode ? tr("&Restart Server") : tr("&Restart"));
    m_Menu->addSeparator();
    QAction* quitAction = m_Menu->addAction(m_ClientMode ? tr("&Quit Server") : tr("&Quit"));

    connect(openAction, &QAction::triggered, this, &TrayManager::onOpen);
    connect(controlPanelAction, &QAction::triggered, this, &TrayManager::onOpenSettings);
    connect(restartAction, &QAction::triggered, this, &TrayManager::onRestart);
    connect(quitAction, &QAction::triggered, this, &TrayManager::onQuit);
    // Booth mode: nothing here may change or stop the server. Hidden rather
    // than left out, so the Dock menu below, which shares them, follows.
    if (m_AdminLocked) {
        controlPanelAction->setVisible(false);
        restartAction->setVisible(false);
        quitAction->setVisible(false);
    }
    connect(m_TrayIcon, &QSystemTrayIcon::activated, this, &TrayManager::onActivated);

    m_TrayIcon->setContextMenu(m_Menu);
    m_TrayIcon->show();
#ifdef Q_OS_WIN
    noticeIcon(); // rendered now, at startup, rather than at the first stream's start
#endif

#ifdef Q_OS_MACOS
    // Dock right-click menu — reuse the tray actions; macOS appends its own
    // Quit entry, so ours is omitted here.
    m_DockMenu = new QMenu();
    m_DockMenu->addAction(m_StreamingAction); // hides itself with the tray entry
    m_DockMenu->addAction(openAction);
    m_DockMenu->addAction(controlPanelAction);
    m_DockMenu->addSeparator();
    if (m_AutostartAction) {
        m_DockMenu->addAction(m_AutostartAction);
        connect(m_DockMenu, &QMenu::aboutToShow, this, &TrayManager::refreshAutostart);
    }
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

    if (m_Activity) {
        m_ActivityTimer.setInterval(m_ActivityIntervalMs);
        connect(&m_ActivityTimer, &QTimer::timeout, this, &TrayManager::pollActivity);
        m_ActivityTimer.start();
        pollActivity(); // adopt whatever is already running, silently
    }

    // Query stripped: the entry URL carries the single-use host key (?mwk=...),
    // which has no business in a log file.
    QUrl logged = localUrl(QString());
    logged.setQuery(QString());
    qInfo() << "[TrayManager] System tray icon created" << (m_ClientMode ? "(client mode)" : "")
            << "for" << logged.toString();
    return true;
}

// "MoonlightWeb" alone when nobody is watching; the count on a second line when
// someone is. Windows shows both lines, and every platform shows the first.
void TrayManager::refreshTooltip(int count)
{
    if (!m_TrayIcon) return;
    QString tip = mw::edition::displayName();
    if (count > 0) tip += QLatin1Char('\n') + tr("Streaming (%1)").arg(count);
    m_TrayIcon->setToolTip(tip);
}

void TrayManager::notify(const QString& text)
{
#ifdef Q_OS_WIN
    m_TrayIcon->showMessage(mw::edition::displayName(), text, noticeIcon(), 5000);
#else
    // Elsewhere the type itself is read: the icon name and the urgency of a
    // freedesktop notification come from it.
    m_TrayIcon->showMessage(mw::edition::displayName(), text, QSystemTrayIcon::Information, 5000);
#endif
}

// The information icon of a notification, rendered once per device pixel ratio.
//
// Handed the style's icon, Qt for Windows renders it anew for every notice, at
// up to 256 px. That work runs on two threads of Qt's GUI pool, which end 30 s
// later, each with a hidden window. A window created or destroyed anywhere in
// the session holds the next SendInput press for up to one DWM frame (D29,
// 07/11/2026). The arrival notice comes at a stream's start, so its two threads
// ended half a minute into the stream. Kept, the same pixmap shows the same
// notice and is never drawn again.
QIcon TrayManager::noticeIcon()
{
    const qreal dpr = qApp->devicePixelRatio();
    if (m_NoticeIcon.isNull() || m_NoticeIconDpr != dpr) {
        const QIcon style = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);
        // The size Qt for Windows asks of the icon a notice is given.
        m_NoticeIcon = QIcon(style.pixmap(style.actualSize(QSize(256, 256))));
        m_NoticeIconDpr = dpr;
    }
    return m_NoticeIcon;
}

// One poll, three consequences: the tooltip, the menu entry, and a notification
// for each viewer who arrived or left since the last one.
//
// The comparison is by viewer id, not by count: two people swapping places
// between two polls leaves the count at 1, and both facts are worth a word. And
// a viewer only ever produces one notification per event, so a stream that
// merely changes quality (the standby leg is the same person continuing) says
// nothing — the provider is what guarantees that, by reporting people rather
// than sessions.
void TrayManager::pollActivity()
{
    if (!m_Activity || !m_TrayIcon) return;
    const StreamActivity activity = m_Activity();
    if (!activity.valid) return; // no answer is not an answer — keep what we had

    const ViewerNotices::Changes changes =
        m_Notices.observe(activity.viewers, QDateTime::currentDateTime(), activity.notify);

    // Logged as well as shown, because whether the desktop actually DISPLAYS a
    // notification is not ours to decide: Do Not Disturb, focus assist and the
    // shell's own rate limiting all swallow them silently, and without a line
    // here "no notification appeared" cannot be told from "none was sent".
    if (activity.notify) {
        for (const StreamViewer& v : changes.arrived)
            qInfo() << "[TrayManager] Notifying: arrival of" << ViewerNotices::nameOf(v);
        for (const StreamViewer& v : changes.left)
            qInfo() << "[TrayManager] Notifying: departure of" << ViewerNotices::nameOf(v);
    }
    releaseNotice();

    // The dot says "someone is watching" on its own, whatever became of the
    // notices: a wrong reading of Windows' state, or a balloon nobody saw,
    // still leaves it there until the last viewer has gone.
    const bool watched = m_Notices.watched();
    if (watched != m_ShowsDot) {
        m_ShowsDot = watched;
        m_TrayIcon->setIcon(watched ? m_IconWatched : m_Icon);
    }

    refreshTooltip(activity.count());
    if (m_StreamingAction) {
        m_StreamingAction->setText(tr("Streaming (%1)").arg(activity.count()));
        m_StreamingAction->setVisible(activity.count() > 0);
    }
    const bool anyRecent = !m_Notices.recent().isEmpty();
    if (m_RecentMenu) m_RecentMenu->menuAction()->setVisible(anyRecent);
    if (m_StreamingSeparator) m_StreamingSeparator->setVisible(activity.count() > 0 || anyRecent);
}

// One notice at a time, and only once Windows says it would show it.
//
// SHQueryUserNotificationState is read before each: a full-screen game, a
// presentation or quiet hours make the notice wait (A137). Waiting is logged
// once when it starts, and each notice let out after a wait once — never with a
// name: who watched is the tray's to say, not the log's.
void TrayManager::releaseNotice()
{
    const qint64 nowMs = m_Clock.elapsed();
    if (!m_Notices.due(nowMs)) return;
#ifdef Q_OS_WIN
    QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
    const bool ok = SUCCEEDED(SHQueryUserNotificationState(&state));
    const bool shows = ViewerNotices::windowsShows(ok, static_cast<int>(state));
#else
    // Elsewhere the notification service applies its own do-not-disturb, and
    // there is no state to ask for: a notice always goes out.
    const int state = 5;
    const bool shows = true;
#endif
    if (!shows) {
        if (m_Notices.hold())
            qInfo() << "[TrayManager] Notice held (state" << static_cast<int>(state) << "),"
                    << m_Notices.waiting() << "waiting";
        return;
    }
    const bool wasHeld = m_Notices.nextWasHeld();
    notify(m_Notices.take(QDateTime::currentDateTime(), nowMs));
    if (wasHeld)
        qInfo() << "[TrayManager] Held notice released," << m_Notices.waiting() << "waiting";
    static int droppedSaid = 0;
    if (m_Notices.dropped() != droppedSaid) {
        droppedSaid = m_Notices.dropped();
        qWarning() << "[TrayManager]" << droppedSaid
                   << "waiting notice(s) dropped, oldest first, over" << ViewerNotices::kMaxPeople
                   << "people";
    }
}

void TrayManager::refreshRecentMenu()
{
    if (!m_RecentMenu) return;
    m_RecentMenu->clear();
    const QDateTime now = QDateTime::currentDateTime();
    for (const ViewerNotices::Viewing& v : m_Notices.recent())
        m_RecentMenu->addAction(m_Notices.recentLine(v, now))->setEnabled(false);
}

// The app's icon with a red dot in its lower right corner, white-ringed so it
// reads on a dark taskbar and a light one alike. Painted once, at init, for
// every size a tray asks for: never while somebody is streaming (see noticeIcon
// for what drawing then costs).
QIcon TrayManager::withDot(const QIcon& base)
{
    QIcon out;
    for (int size : {16, 20, 24, 32, 40, 48, 64}) {
        QPixmap pm = base.pixmap(size, size);
        if (pm.isNull()) continue;
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal d = size * 0.5;
        const QRectF dot(size - d, size - d, d, d);
        p.setPen(QPen(Qt::white, qMax<qreal>(1.0, size / 16.0)));
        p.setBrush(QColor(0xE5, 0x1A, 0x1A));
        p.drawEllipse(dot.adjusted(0.5, 0.5, -0.5, -0.5));
        p.end();
        out.addPixmap(pm);
    }
    return out.isNull() ? base : out;
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

// One entry, two possible addresses, and the choice is not the user's to make.
//
// The internet link is preferred whenever there is one, for a single reason: it
// is the only address that reaches these pages under a certificate the browser
// already trusts. Loopback never will be — it is served by the self-signed
// certificate, and every visit costs an acceptance or a scary interstitial.
//
// The provider answers empty whenever that link would not work: the option is
// off, the line is down, or the introduction server did not respond when asked.
// So the fallback is not a guess — by the time we reach it, the internet has
// already been tried and found wanting, and loopback is what is left that
// always works. Nothing is said about it either way; the owner asked for a page,
// not for a report on how it was reached.
void TrayManager::openAppPage(const QString& path)
{
    const QString link = m_RemoteLink ? m_RemoteLink(path) : QString();
    if (!link.isEmpty()) {
        // Logged without its fragment. The key in there is single-use, but it is
        // still a credential, and a log file is exactly the place it must not
        // outlive its one use in.
        QUrl logged(link);
        logged.setFragment(QString());
        qInfo() << "[TrayManager] Opening" << (path.isEmpty() ? QStringLiteral("/") : path)
                << "by the owner's link" << logged.toString();
        QDesktopServices::openUrl(QUrl(link));
        return;
    }

    QUrl url = localUrl(path);
    if (url.isEmpty()) {
        qWarning() << "[TrayManager] Cannot open — no HTTP/HTTPS listener running";
        return;
    }
    qInfo() << "[TrayManager] Opening" << url.toString();
    QDesktopServices::openUrl(url);
}

void TrayManager::onOpen()
{
    openAppPage(QString());
}

void TrayManager::onOpenSettings()
{
    if (m_AdminLocked) {
        onOpen(); // no settings page in booth mode: the Dock click lands here
        return;
    }
    openAppPage(QStringLiteral("/admin"));
}

// The admin page, scrolled to its Sessions table — which is the only place that
// answers the question the menu entry just raised: who, since when, from where.
//
// Its own path rather than a fragment on /admin: the internet link carries the
// single-use host key in the fragment, and there is exactly one of those.
void TrayManager::onOpenSessions()
{
    openAppPage(QStringLiteral("/sessions"));
}

// Blocked so the OS probe does not come back as a toggle: setChecked() emits
// toggled(), and that would (re)install what was only being read.
void TrayManager::refreshAutostart()
{
    if (!m_AutostartAction) return;
    const QSignalBlocker quiet(m_AutostartAction);
    m_AutostartAction->setChecked(Autostart::isLoginItemInstalled());
}

// The tick is the request, the OS is the answer: on failure the tick goes back
// to what is actually true, and the log says why — a box that stays ticked
// after a refused write would be a promise nobody made.
void TrayManager::onAutostartToggled(bool on)
{
    qInfo() << "[TrayManager]" << (on ? "Enabling" : "Disabling") << "start at login";
    const bool ok = on ? Autostart::installLoginItem() : Autostart::removeLoginItem();
    if (!ok) {
        qWarning() << "[TrayManager] Start at login could not be" << (on ? "enabled" : "disabled");
        if (m_TrayIcon)
            m_TrayIcon->showMessage(mw::edition::displayName(),
                                    on ? tr("Could not enable start at login")
                                       : tr("Could not disable start at login"),
                                    QSystemTrayIcon::Warning, 5000);
    }
    refreshAutostart();
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
            m_TrayIcon->showMessage(mw::edition::displayName(), tr("Restarting the server…"),
                                    QSystemTrayIcon::Information, 3000);
        m_RestartServer();
        return;
    }

    qInfo() << "[TrayManager] Restarting application...";

    // Launch a new instance with the same arguments, then quit this one
    QString appPath = QCoreApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) args.removeFirst(); // argv[0] is the app path, startDetached handles it

    if (!mw::startDetached(appPath, args)) {
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
