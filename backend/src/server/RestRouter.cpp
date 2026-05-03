#include "RestRouter.h"
#include <QJsonDocument>

HttpResponse HttpResponse::json(const QJsonObject& obj, int status)
{
    HttpResponse resp;
    resp.statusCode = status;
    resp.contentType = "application/json";
    resp.body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return resp;
}

HttpResponse HttpResponse::text(const QString& text, int status)
{
    HttpResponse resp;
    resp.statusCode = status;
    resp.contentType = "text/plain; charset=utf-8";
    resp.body = text.toUtf8();
    return resp;
}

HttpResponse HttpResponse::error(int status, const QString& message)
{
    QJsonObject obj;
    obj["error"] = message;
    obj["status"] = status;
    return json(obj, status);
}

RestRouter::RestRouter(QObject* parent)
    : QObject(parent)
{
}

void RestRouter::get(const QString& path, RouteHandler handler)
{
    m_Routes[routeKey("GET", path)] = std::move(handler);
}

void RestRouter::post(const QString& path, RouteHandler handler)
{
    m_Routes[routeKey("POST", path)] = std::move(handler);
}

void RestRouter::del(const QString& path, RouteHandler handler)
{
    m_Routes[routeKey("DELETE", path)] = std::move(handler);
}

HttpResponse RestRouter::dispatch(const HttpRequest& request) const
{
    QString key = routeKey(request.method, request.path);
    if (m_Routes.contains(key))
        return m_Routes[key](request);

    // Try exact path match first, then fallback
    for (auto it = m_Routes.cbegin(); it != m_Routes.cend(); ++it) {
        if (it.key().startsWith(request.method + ":")) {
            QString pattern = it.key().mid(request.method.length() + 1);
            if (pattern == request.path)
                return it.value()(request);
        }
    }

    return HttpResponse::error(404, "Not Found: " + request.method + " " + request.path);
}

bool RestRouter::hasRoute(const QString& method, const QString& path) const
{
    return m_Routes.contains(routeKey(method, path));
}

QString RestRouter::routeKey(const QString& method, const QString& path) const
{
    return method + ":" + path;
}
