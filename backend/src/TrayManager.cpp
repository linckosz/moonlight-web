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
#include "common/Edition.h"
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
    m_TrayIcon->setIcon(icon);

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
    m_StreamingSeparator = m_Menu->addSeparator();
    m_StreamingSeparator->setVisible(false);
    connect(m_StreamingAction, &QAction::triggered, this, &TrayManager::onOpenSessions);

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
    m_DockMenu->addAction(m_StreamingAction); // hides itself with the tray entry
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

// What to call a viewer in a notification.
//
// The device name from the sessions table, except for a browser on this very
// machine: its row reads "Host machine (remote link)", which distinguishes it
// from the other rows and says nothing to somebody reading a notice on that
// machine. And "Someone" when there is no name at all — a local stream carries
// no session row to take one from.
QString TrayManager::viewerName(const StreamViewer& v)
{
    if (v.self) return tr("This computer");
    return v.name.isEmpty() ? tr("Someone") : v.name;
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

    QSet<QString> ids;
    for (const StreamViewer& v : activity.viewers)
        ids.insert(v.id);

    // Logged as well as shown, because whether the desktop actually DISPLAYS a
    // notification is not ours to decide: Do Not Disturb, focus assist and the
    // shell's own rate limiting all swallow them silently, and without a line
    // here "no notification appeared" cannot be told from "none was sent".
    if (m_ViewersKnown && activity.notify) {
        for (const StreamViewer& v : activity.viewers) {
            if (m_Viewers.contains(v.id)) continue;
            qInfo() << "[TrayManager] Notifying: arrival of" << viewerName(v);
            m_TrayIcon->showMessage(mw::edition::displayName(),
                                    tr("%1 started streaming this screen").arg(viewerName(v)),
                                    QSystemTrayIcon::Information, 5000);
        }
        // Departures are named from the previous snapshot, which is the only
        // place a viewer who is gone still has a name.
        for (const StreamViewer& v : m_LastViewers) {
            if (ids.contains(v.id)) continue;
            qInfo() << "[TrayManager] Notifying: departure of" << viewerName(v);
            m_TrayIcon->showMessage(mw::edition::displayName(),
                                    tr("%1 stopped streaming this screen").arg(viewerName(v)),
                                    QSystemTrayIcon::Information, 5000);
        }
    }

    m_Viewers = ids;
    m_LastViewers = activity.viewers;
    m_ViewersKnown = true;

    refreshTooltip(activity.count());
    if (m_StreamingAction) {
        m_StreamingAction->setText(tr("Streaming (%1)").arg(activity.count()));
        m_StreamingAction->setVisible(activity.count() > 0);
        if (m_StreamingSeparator) m_StreamingSeparator->setVisible(activity.count() > 0);
    }
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
                << "over the internet link" << logged.toString();
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
