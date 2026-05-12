#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>

class HttpServer;

class TrayManager : public QObject
{
    Q_OBJECT

public:
    explicit TrayManager(HttpServer* server, QObject* parent = nullptr);
    ~TrayManager();

    /// Create and show the tray icon. Returns false if the system tray is unavailable.
    bool init();

private slots:
    void onActivated(QSystemTrayIcon::ActivationReason reason);
    void onOpen();
    void onRestart();
    void onQuit();

private:
    HttpServer* m_Server;
    QSystemTrayIcon* m_TrayIcon;
    QMenu* m_Menu;
};
