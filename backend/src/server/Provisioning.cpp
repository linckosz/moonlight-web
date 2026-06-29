/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#include "Provisioning.h"

#include "AppSettings.h"
#include "../backend/ComputerManager.h"
#include "../backend/NvComputer.h"
#include "../backend/SunshineRestClient.h"
#include "../common/Logger.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace Provisioning {

// Pair the local Sunshine over the GameStream protocol while feeding the PIN
// through Sunshine's REST API (no manual entry in Sunshine's web UI).
static void pairLocalSunshine(ComputerManager& computers, const QString& user, const QString& pass)
{
    auto [addStatus, addResult] = computers.handleAddManualHost(QStringLiteral("127.0.0.1"));
    if (addStatus != 200) {
        Logger::warning(QStringLiteral("Provisioning: cannot reach local Sunshine: %1")
                            .arg(addResult.value(QStringLiteral("message")).toString()));
        return;
    }

    const QJsonArray hosts = addResult.value(QStringLiteral("hosts")).toArray();
    if (hosts.isEmpty()) return;
    const QString uuid = hosts.first().toObject().value(QStringLiteral("uuid")).toString();
    if (uuid.isEmpty()) return;

    NvComputer* host = computers.getHost(uuid);
    if (host && host->pairState == NvComputer::PS_PAIRED) {
        Logger::info(QStringLiteral("Provisioning: local Sunshine already paired"));
        return;
    }

    auto [startStatus, startResult] = computers.handleStartPairing(uuid);
    if (startResult.value(QStringLiteral("status")).toString() != QLatin1String("initiated")) {
        Logger::warning(QStringLiteral("Provisioning: pairing could not start: %1")
                            .arg(startResult.value(QStringLiteral("message")).toString()));
        return;
    }
    const QString pin = startResult.value(QStringLiteral("pin")).toString();
    const quint16 restPort = (host && host->activeHttpsPort > 0) ? host->activeHttpsPort : 47990;

    // handleSubmitPin() blocks on getservercert (a nested Qt event loop) until
    // Sunshine receives the PIN. Schedule the REST push so it fires *during*
    // that block — getservercert must already be in flight for Sunshine to
    // attach the PIN to the pending pairing request.
    auto* rest = new SunshineRestClient(&computers);
    QTimer::singleShot(800, rest, [rest, pin, user, pass, restPort]() {
        rest->sendPin(pin, user, pass, QStringLiteral("moonlight-web"), restPort);
    });

    auto [submitStatus, submitResult] = computers.handleSubmitPin(uuid);
    Logger::info(QStringLiteral("Provisioning: local Sunshine pairing -> %1 (%2)")
                     .arg(submitResult.value(QStringLiteral("status")).toString(),
                          submitResult.value(QStringLiteral("message")).toString()));
}

void applyOnce(const QString& exeDir, AppSettings& settings, ComputerManager& computers)
{
    const QString path = exeDir + QStringLiteral("/provisioning.json");
    QFile file(path);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::warning(QStringLiteral("Provisioning: cannot read %1").arg(path));
        return;
    }

    QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    Logger::info(QStringLiteral("Provisioning: applying %1").arg(path));

    // Internet Access: just flip the persisted flag; main()'s existing
    // auto-start path brings the InternetAccessManager up after this returns.
    if (obj.value(QStringLiteral("internet_access_authorized")).toBool()) {
        settings.setInternetAccessEnabled(true);
        Logger::info(QStringLiteral("Provisioning: Internet Access authorized"));
    }

    const QJsonObject sun = obj.value(QStringLiteral("sunshine")).toObject();
    if (sun.value(QStringLiteral("auto_pair")).toBool()) {
        const QString user = sun.value(QStringLiteral("username")).toString(QStringLiteral("admin"));
        const QString pass = sun.value(QStringLiteral("password")).toString(QStringLiteral("admin"));
        pairLocalSunshine(computers, user, pass);
    }

    // Consume: rewrite without the password, then remove the original so the
    // plaintext credential never lingers and provisioning is not replayed.
    QJsonObject consumedSun = sun;
    consumedSun.remove(QStringLiteral("password"));
    if (!consumedSun.isEmpty()) obj[QStringLiteral("sunshine")] = consumedSun;
    QFile consumed(exeDir + QStringLiteral("/provisioning.consumed.json"));
    if (consumed.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        consumed.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        consumed.close();
    }
    QFile::remove(path);
}

} // namespace Provisioning
