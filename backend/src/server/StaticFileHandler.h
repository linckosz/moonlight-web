#pragma once

#include <QObject>
#include <QMap>
#include "common/Types.h"

class StaticFileHandler : public QObject
{
    Q_OBJECT

public:
    explicit StaticFileHandler(const QString& rootDir, QObject* parent = nullptr);

    HttpResponse serveFile(const QString& requestPath) const;

private:
    QString mimeType(const QString& path) const;
    static QMap<QString, QString> s_MimeTypes;

    QString m_RootDir;
};
