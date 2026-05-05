#pragma once

#include <QObject>
#include <QMap>
#include <QStringList>
#include <functional>
#include "common/Types.h"

using SyncRouteHandler = std::function<HttpResponse(const HttpRequest&)>;
using AsyncRouteHandler = std::function<void(const HttpRequest&, ResponseCallback)>;

class RestRouter : public QObject
{
    Q_OBJECT

public:
    explicit RestRouter(QObject* parent = nullptr);

    // Sync handlers — wrapped internally to call respond() synchronously
    void get(const QString& path, SyncRouteHandler handler);
    void post(const QString& path, SyncRouteHandler handler);
    void del(const QString& path, SyncRouteHandler handler);

    // Async handlers — for routes that need deferred responses
    void getAsync(const QString& path, AsyncRouteHandler handler);
    void postAsync(const QString& path, AsyncRouteHandler handler);

    // Dispatch — calls respond() synchronously for sync routes, asynchronously otherwise
    void dispatchAsync(const HttpRequest& request, ResponseCallback respond) const;
    bool hasRoute(const QString& method, const QString& path) const;

private:
    struct ParamRoute {
        QString method;
        QStringList segments;
        AsyncRouteHandler handler;
    };

    QString routeKey(const QString& method, const QString& path) const;
    bool matchParamRoute(const QStringList& pathSegments, const ParamRoute& route,
                         QMap<QString, QString>& outParams) const;

    QMap<QString, AsyncRouteHandler> m_Routes;
    QList<ParamRoute> m_ParamRoutes;
};
