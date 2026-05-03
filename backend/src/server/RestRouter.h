#pragma once

#include <QObject>
#include <QMap>
#include <QJsonObject>
#include <functional>

struct HttpRequest {
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QMap<QString, QString> queryParams;
    QByteArray body;
};

struct HttpResponse {
    int statusCode = 200;
    QString contentType;
    QMap<QString, QString> headers;
    QByteArray body;

    static HttpResponse json(const QJsonObject& obj, int status = 200);
    static HttpResponse text(const QString& text, int status = 200);
    static HttpResponse error(int status, const QString& message);
};

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
