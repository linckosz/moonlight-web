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

#include "common/Edition.h"

#include <cstring>

// Baked in by CMake. Fallback keeps the test runner (which does not define it)
// and tooling builds compiling.
#ifndef MW_VERSION
#define MW_VERSION "0.0.0-dev"
#endif

namespace mw::edition {

namespace {

bool g_devFlag = false;

constexpr bool kDevBuild =
#ifdef MW_EDITION_DEV
    true;
#else
    false;
#endif

} // namespace

QString productNameFor(bool devBuild)
{
    return devBuild ? QStringLiteral("MoonlightWebDev") : QStringLiteral("MoonlightWeb");
}

QString dataNameFor(bool devBuild, bool devFlag)
{
    QString name = productNameFor(devBuild);
    if (devFlag) name += QStringLiteral("-dev");
    return name;
}

QString productName()
{
    return productNameFor(kDevBuild);
}

QString displayNameFor(bool devBuild, bool devFlag)
{
    return devBuild || devFlag ? QStringLiteral("MoonlightWebDev") : QStringLiteral("MoonlightWeb");
}

QString versionFor(const QString& base, bool dev)
{
    if (!dev || base.endsWith(QLatin1String("-dev"))) return base;
    return base + QStringLiteral("-dev");
}

void init(int argc, char** argv)
{
    g_devFlag = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--dev") == 0) g_devFlag = true;
}

bool isDevBuild()
{
    return kDevBuild;
}

bool devFlag()
{
    return g_devFlag;
}

bool isDev()
{
    return kDevBuild || g_devFlag;
}

QString dataName()
{
    return dataNameFor(kDevBuild, g_devFlag);
}

QString displayName()
{
    return displayNameFor(kDevBuild, g_devFlag);
}

QString version()
{
    return versionFor(QStringLiteral(MW_VERSION), isDev());
}

quint16 defaultHttpPort()
{
    return isDev() ? kDevHttpPort : quint16(80);
}

quint16 defaultHttpsPort()
{
    return isDev() ? kDevHttpsPort : quint16(443);
}

QString iconAsset(const QString& file)
{
    return (isDev() ? QStringLiteral("assets/dev/") : QStringLiteral("assets/")) + file;
}

bool lanOnlyFrom(const QByteArray& value)
{
    const QByteArray v = value.trimmed().toLower();
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool lanOnly()
{
    QByteArray value = qgetenv("MW_LAN_ONLY");
#ifdef MW_LAN_ONLY
    // Embedded at configure time (backend/CMakeLists.txt); the environment wins.
    if (value.isEmpty()) value = QByteArray(MW_LAN_ONLY);
#endif
    return lanOnlyFrom(value);
}

QString lanOnlyRefusal()
{
    return QStringLiteral("This instance is LAN-only (MW_LAN_ONLY is set): Internet access "
                          "cannot be enabled. Open it at its LAN address instead.");
}

} // namespace mw::edition
