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

#include "IStreamBackend.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

// Type name -> provider factory.
//
// Adding a backend (MultiSeat, Wolf, punktfunk, …) is meant to be one new
// IStreamBackend subclass plus one registerFactory() call at boot — no changes
// here and none in the call sites.
class StreamBackendRegistry
{
public:
    using Factory = std::function<std::unique_ptr<IStreamBackend>(const QJsonObject& config)>;

    static StreamBackendRegistry& instance();

    // Last registration for a given type wins; re-registering is a programming
    // error and is logged.
    void registerFactory(const QString& type, Factory factory);

    // Returns nullptr when the type was never registered.
    std::unique_ptr<IStreamBackend> create(const QString& type, const QJsonObject& config) const;

    bool isRegistered(const QString& type) const;
    QStringList knownTypes() const;

private:
    StreamBackendRegistry() = default;

    QMap<QString, Factory> m_Factories;
};
