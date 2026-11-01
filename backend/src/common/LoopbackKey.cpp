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

#include "common/LoopbackKey.h"

#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QWebSocket>

namespace mw::loopback {

namespace {

QMutex g_Mutex;
QByteArray g_Key;

QByteArray draw()
{
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()),
                                          bytes.size() / qsizetype(sizeof(quint32)));
    return bytes.toHex();
}

} // namespace

QByteArray key()
{
    QMutexLocker lock(&g_Mutex);
    if (g_Key.isEmpty()) g_Key = draw();
    return g_Key;
}

void setKey(const QByteArray& key)
{
    QMutexLocker lock(&g_Mutex);
    // Nothing given (a parent that sends none): a key nobody holds.
    g_Key = key.isEmpty() ? draw() : key;
}

QByteArray stampUpgrade(const QByteArray& upgradeRequest, const QByteArray& key)
{
    const qsizetype end = upgradeRequest.indexOf("\r\n\r\n");
    if (end < 0) return upgradeRequest;

    const QList<QByteArray> lines = upgradeRequest.left(end).split('\n');
    const QByteArray name = QByteArray(kHeader).toLower() + ':';
    QByteArray stamped;
    stamped.reserve(upgradeRequest.size() + key.size() + 32);
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines[i].endsWith('\r') ? lines[i].chopped(1) : lines[i];
        if (i > 0 && line.trimmed().toLower().startsWith(name)) continue;
        stamped += line + "\r\n";
    }
    stamped += QByteArray(kHeader) + ": " + key + "\r\n";
    stamped += upgradeRequest.mid(end + 2);
    return stamped;
}

bool accepts(const QWebSocket& socket, const QByteArray& key)
{
    return !key.isEmpty() && socket.request().rawHeader(kHeader) == key;
}

} // namespace mw::loopback
