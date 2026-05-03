#pragma once

#include <QObject>
#include <QMap>
#include <functional>
#include "common/Types.h"

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

class RestRouter : public QObject
{
    Q_OBJECT

public:
    explicit RestRouter(QObject* parent = nullptr);

    void get(const QString& path, RouteHandler handler);
    void post(const QString& path, RouteHandler handler);
    void del(const QString& path, RouteHandler handler);

    HttpResponse dispatch(const HttpRequest& request) const;
    bool hasRoute(const QString& method, const QString& path) const;

private:
    QString routeKey(const QString& method, const QString& path) const;
    QMap<QString, RouteHandler> m_Routes;
};
