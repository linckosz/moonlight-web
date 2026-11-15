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

#include "common/Logger.h"

#include <QJsonObject>
#include <QList>
#include <QString>

/// One line of moonlightweb-client.log, with its level.
struct ClientLogLine
{
    Logger::Level level;
    QString text;
};

/// What moonlightweb-client.log gets of one batch of a page's console
/// (routes/LogRoutes.cpp): its lines in order, each tagged with the page's id,
/// @p who sent it and from @p address. The first batch (seq 0) opens with the
/// page's address and user agent. Share links are written /p/(link), and the
/// value of a token= or a JSON "token" (hidden), throughout
/// (LogScrubber::hideShareLinks).
QList<ClientLogLine> clientLogLines(const QJsonObject& body, const QString& who,
                                    const QString& address);
