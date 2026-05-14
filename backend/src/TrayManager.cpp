#include "TrayManager.h"
#include "server/HttpServer.h"

#include <QApplication>
#include <QStyle>
#include <QDesktopServices>
#include <QUrl>
#include <QIcon>
#include <QProcess>
#include <QCoreApplication>
#include <QAction>

TrayManager::TrayManager(HttpServer* server, QObject* parent)
    : QObject(parent)
    , m_Server(server)
    , m_TrayIcon(nullptr)
    , m_Menu(nullptr)
{
}

TrayManager::~TrayManager()
{
    if (m_TrayIcon)
        m_TrayIcon->hide();
}

bool TrayManager::init()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qInfo() << "[TrayManager] System tray not available (headless or no desktop)";
        return false;
    }

    m_TrayIcon = new QSystemTrayIcon(this);
    m_Menu = new QMenu();

    // Load tray icon — try frontend favicon first, fall back to standard icon
    QIcon icon(QStringLiteral(FRONTEND_DIR "assets/favicon.ico"));
    if (icon.isNull()) {
        qInfo() << "[TrayManager] favicon.ico not found, using standard icon";
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    m_TrayIcon->setIcon(icon);

    quint16 port = m_Server->activeHttpsPort();
    m_TrayIcon->setToolTip(QStringLiteral("Moonlight-Web\nhttps://localhost:%1").arg(port));

    // Build context menu
    QAction* openAction = m_Menu->addAction(tr("&Open"));
    QAction* controlPanelAction = m_Menu->addAction(tr("&Control Panel"));
    m_Menu->addSeparator();
    QAction* restartAction = m_Menu->addAction(tr("&Restart"));
    m_Menu->addSeparator();
    QAction* quitAction = m_Menu->addAction(tr("&Quit"));

    connect(openAction, &QAction::triggered, this, &TrayManager::onOpen);
    connect(controlPanelAction, &QAction::triggered, this, &TrayManager::onOpen);
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
    if (reason == QSystemTrayIcon::DoubleClick)
        onOpen();
}

void TrayManager::onOpen()
{
    quint16 port = m_Server->activeHttpsPort();
    if (port == 0) {
        qWarning() << "[TrayManager] Cannot open — HTTPS server not running";
        return;
    }
    QUrl url(QStringLiteral("https://localhost:%1").arg(port));
    qInfo() << "[TrayManager] Opening" << url.toString();
    QDesktopServices::openUrl(url);
}

void TrayManager::onRestart()
{
    qInfo() << "[TrayManager] Restarting application...";

    // Launch a new instance with the same arguments, then quit this one
    QString appPath = QCoreApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty())
        args.removeFirst();  // argv[0] is the app path, startDetached handles it

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
