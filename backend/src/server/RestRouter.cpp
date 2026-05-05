#include "RestRouter.h"

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

void RestRouter::get(const QString& path, SyncRouteHandler handler)
{
    // Wrap sync → async
    getAsync(path, [h = std::move(handler)](const HttpRequest& req, ResponseCallback respond) {
        respond(h(req));
    });
}

void RestRouter::post(const QString& path, SyncRouteHandler handler)
{
    postAsync(path, [h = std::move(handler)](const HttpRequest& req, ResponseCallback respond) {
        respond(h(req));
    });
}

void RestRouter::del(const QString& path, SyncRouteHandler handler)
{
    AsyncRouteHandler wrapped = [h = std::move(handler)](const HttpRequest& req, ResponseCallback respond) {
        respond(h(req));
    };

    if (path.contains(':')) {
        ParamRoute r;
        r.method = "DELETE";
        r.segments = path.split('/', Qt::SkipEmptyParts);
        r.handler = std::move(wrapped);
        m_ParamRoutes.append(std::move(r));
    } else {
        m_Routes[routeKey("DELETE", path)] = std::move(wrapped);
    }
}

void RestRouter::getAsync(const QString& path, AsyncRouteHandler handler)
{
    if (path.contains(':')) {
        ParamRoute r;
        r.method = "GET";
        r.segments = path.split('/', Qt::SkipEmptyParts);
        r.handler = std::move(handler);
        m_ParamRoutes.append(std::move(r));
    } else {
        m_Routes[routeKey("GET", path)] = std::move(handler);
    }
}

void RestRouter::postAsync(const QString& path, AsyncRouteHandler handler)
{
    if (path.contains(':')) {
        ParamRoute r;
        r.method = "POST";
        r.segments = path.split('/', Qt::SkipEmptyParts);
        r.handler = std::move(handler);
        m_ParamRoutes.append(std::move(r));
    } else {
        m_Routes[routeKey("POST", path)] = std::move(handler);
    }
}

bool RestRouter::matchParamRoute(const QStringList& pathSegments,
                                  const ParamRoute& route,
                                  QMap<QString, QString>& outParams) const
{
    if (pathSegments.size() != route.segments.size())
        return false;

    QMap<QString, QString> params;

    for (int i = 0; i < pathSegments.size(); ++i) {
        const QString& pattern = route.segments[i];
        const QString& actual = pathSegments[i];

        if (pattern.startsWith(':')) {
            QString key = pattern.mid(1);
            if (actual.isEmpty())
                return false;
            params[key] = actual;
        } else if (pattern != actual) {
            return false;
        }
    }

    outParams = params;
    return true;
}

void RestRouter::dispatchAsync(const HttpRequest& request, ResponseCallback respond) const
{
    // Fast path: exact match
    QString key = routeKey(request.method, request.path);
    if (m_Routes.contains(key)) {
        m_Routes[key](request, std::move(respond));
        return;
    }

    // Slow path: parameterized routes
    QStringList pathSegments = request.path.split('/', Qt::SkipEmptyParts);

    for (const auto& route : m_ParamRoutes) {
        if (route.method != request.method)
            continue;

        QMap<QString, QString> params;
        if (matchParamRoute(pathSegments, route, params)) {
            HttpRequest reqCopy = request;
            reqCopy.pathParams = params;
            route.handler(reqCopy, std::move(respond));
            return;
        }
    }

    respond(HttpResponse::error(404, "Not Found: " + request.method + " " + request.path));
}

bool RestRouter::hasRoute(const QString& method, const QString& path) const
{
    if (m_Routes.contains(routeKey(method, path)))
        return true;

    QStringList segments = path.split('/', Qt::SkipEmptyParts);
    for (const auto& route : m_ParamRoutes) {
        if (route.method != method)
            continue;
        QMap<QString, QString> params;
        if (matchParamRoute(segments, route, params))
            return true;
    }
    return false;
}

QString RestRouter::routeKey(const QString& method, const QString& path) const
{
    return method + ":" + path;
}
