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

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkRequest>

// A QNetworkAccessManager whose connections end with their request.
//
// Qt keeps an idle HTTP connection for 120 s, to reuse it. On Windows each one
// also holds a network-status monitor, which keeps the COM apartment of the
// manager's thread open, and the apartment's hidden window goes only with the
// last connection. Those 120 s outlived the host poll that opened them and ran
// into the next stream. There the window's end held the next SendInput press
// for up to one DWM frame: a window created or destroyed anywhere in the
// session does that (D29, 07/11/2026). The poll pauses while a stream runs, so
// with nothing kept the window comes and goes between streams only.
//
// Sunshine's single-threaded HTTP server wants the same (NvHTTP::closeWhenDone).
// This makes it the rule for every request, the ones that forgot it included.
// A request that sets its own expiry keeps it.
class CloseWhenDoneNam : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoingData = nullptr) override
    {
        if (request.attribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute)
                .isValid())
            return QNetworkAccessManager::createRequest(op, request, outgoingData);
        QNetworkRequest closing(request);
        closing.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
        return QNetworkAccessManager::createRequest(op, closing, outgoingData);
    }
};
