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

#include "network/UpdateApplyRules.h"

#include "network/UpdateChecker.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace UpdateApplyRules {

Signer signerVerdict(const QString& installedSigner, bool verified, const QString& installerSigner)
{
    if (installedSigner.isEmpty()) return Signer::MayRun;
    if (!verified) return Signer::NotVerified;
    return installerSigner == installedSigner ? Signer::MayRun : Signer::OtherSigner;
}

QString windowsAssetSuffix(bool arm64)
{
    return arm64 ? QStringLiteral("win-arm64.exe") : QStringLiteral("win-x64.exe");
}

QString expectedInstaller(const QJsonObject& release, const QString& productName,
                          const QString& currentVersion, const QString& assetSuffix, Installer* out)
{
    // releases/latest never answers with either, but the answer is what is
    // being judged, not what the endpoint is documented to do.
    if (release.value("draft").toBool() || release.value("prerelease").toBool())
        return QStringLiteral("the latest release is a draft or a pre-release");

    QString version = release.value("tag_name").toString().trimmed();
    if (version.startsWith(QLatin1Char('v')) || version.startsWith(QLatin1Char('V')))
        version = version.mid(1);
    if (version.isEmpty()) return QStringLiteral("the latest release has no tag");
    // Strictly newer: a genuine but older installer is still a way to put back
    // a version with a hole that the current one closed.
    if (!UpdateChecker::isNewer(version, currentVersion))
        return QStringLiteral("the latest release (%1) is not newer than this install (%2)")
            .arg(version, currentVersion);

    // Exactly one candidate. The prefix keeps a DEV installer attached to the
    // same release from ever standing in for the production one, and a second
    // match would mean guessing which of the two the server downloaded.
    const QString prefix = productName + QStringLiteral("-installer-");
    QJsonObject asset;
    int found = 0;
    for (const QJsonValue& v : release.value("assets").toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value("name").toString();
        if (name.startsWith(prefix, Qt::CaseInsensitive) &&
            name.endsWith(assetSuffix, Qt::CaseInsensitive)) {
            asset = a;
            ++found;
        }
    }
    if (found == 0)
        return QStringLiteral("release %1 has no %2*%3 asset").arg(version, prefix, assetSuffix);
    if (found > 1)
        return QStringLiteral("release %1 has %2 assets matching %3*%4")
            .arg(version)
            .arg(found)
            .arg(prefix, assetSuffix);

    const qint64 size = static_cast<qint64>(asset.value("size").toDouble());
    if (size <= 0)
        return QStringLiteral("GitHub gives no size for %1").arg(asset["name"].toString());

    static const QRegularExpression sha(QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
    const QRegularExpressionMatch m = sha.match(asset.value("digest").toString().trimmed());
    if (!m.hasMatch())
        return QStringLiteral("GitHub gives no sha256 digest for %1").arg(asset["name"].toString());

    out->version = version;
    out->name = asset.value("name").toString();
    out->size = size;
    out->sha256 = m.captured(1).toLower();
    return {};
}

QString matches(const Installer& expected, qint64 size, const QString& sha256Hex)
{
    if (expected.sha256.size() != 64) return QStringLiteral("no digest to compare against");
    if (size != expected.size)
        return QStringLiteral("the staged installer is %1 bytes, %2 is %3")
            .arg(size)
            .arg(expected.name)
            .arg(expected.size);
    if (sha256Hex.compare(expected.sha256, Qt::CaseInsensitive) != 0)
        return QStringLiteral("the staged installer's sha256 (%1) is not %2's (%3)")
            .arg(sha256Hex.left(12), expected.name, expected.sha256.left(12));
    return {};
}

} // namespace UpdateApplyRules
