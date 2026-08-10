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

#include "StreamBackendRegistry.h"

#include "../../common/Logger.h"

StreamBackendRegistry& StreamBackendRegistry::instance()
{
    static StreamBackendRegistry s_Instance;
    return s_Instance;
}

void StreamBackendRegistry::registerFactory(const QString& type, Factory factory)
{
    if (type.isEmpty() || !factory) {
        Logger::warning(QStringLiteral("Stream backend registry: refused an invalid registration"));
        return;
    }

    if (m_Factories.contains(type)) {
        Logger::warning(
            QStringLiteral("Stream backend registry: '%1' registered twice, overriding").arg(type));
    }

    m_Factories.insert(type, std::move(factory));
}

std::unique_ptr<IStreamBackend> StreamBackendRegistry::create(const QString& type,
                                                              const QJsonObject& config) const
{
    auto it = m_Factories.constFind(type);
    if (it == m_Factories.constEnd()) {
        Logger::warning(QStringLiteral("Stream backend registry: unknown type '%1'").arg(type));
        return nullptr;
    }

    return it.value()(config);
}

bool StreamBackendRegistry::isRegistered(const QString& type) const
{
    return m_Factories.contains(type);
}

QStringList StreamBackendRegistry::knownTypes() const
{
    return m_Factories.keys();
}
