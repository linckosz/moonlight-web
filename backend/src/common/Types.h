#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>

struct HttpRequest {
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QMap<QString, QString> queryParams;
    QMap<QString, QString> pathParams;
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
