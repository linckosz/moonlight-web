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

#include "Provisioning.h"

#include "AppSettings.h"
#include "../backend/ComputerManager.h"
#include "../backend/NvComputer.h"
#include "../backend/SunshineRestClient.h"
#include "../common/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

namespace Provisioning {

// Live checklist the Windows installer polls during its post-install page. Lives
// in the per-user data dir (writable; next to settings.json), NOT next to the
// exe (Program Files is read-only for the user session).
void setStepStatus(const QString& step, const QString& state)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/provisioning.status.json");

    QJsonObject root;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    QJsonObject steps = root.value(QStringLiteral("steps")).toObject();
    steps[step] = state;
    root[QStringLiteral("steps")] = steps;
    root[QStringLiteral("updated")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

// Pair the local Sunshine over the GameStream protocol while feeding the PIN
// through Sunshine's REST API (no manual entry in Sunshine's web UI).
static bool pairLocalSunshine(ComputerManager& computers, const QString& user, const QString& pass)
{
    auto [addStatus, addResult] = computers.handleAddManualHost(QStringLiteral("127.0.0.1"));
    if (addStatus != 200) {
        Logger::warning(QStringLiteral("Provisioning: cannot reach local Sunshine: %1")
                            .arg(addResult.value(QStringLiteral("message")).toString()));
        return false;
    }

    const QJsonArray hosts = addResult.value(QStringLiteral("hosts")).toArray();
    if (hosts.isEmpty()) return false;
    const QString uuid = hosts.first().toObject().value(QStringLiteral("uuid")).toString();
    if (uuid.isEmpty()) return false;

    NvComputer* host = computers.getHost(uuid);
    if (host && host->pairState == NvComputer::PS_PAIRED) {
        Logger::info(QStringLiteral("Provisioning: local Sunshine already paired"));
        return true;
    }

    auto [startStatus, startResult] = computers.handleStartPairing(uuid);
    if (startResult.value(QStringLiteral("status")).toString() != QLatin1String("initiated")) {
        Logger::warning(QStringLiteral("Provisioning: pairing could not start: %1")
                            .arg(startResult.value(QStringLiteral("message")).toString()));
        return false;
    }
    const QString pin = startResult.value(QStringLiteral("pin")).toString();
    const quint16 restPort = (host && host->activeHttpsPort > 0) ? host->activeHttpsPort : 47990;

    // handleSubmitPin() blocks on getservercert (a nested Qt event loop) until
    // Sunshine receives the PIN. Schedule the REST push so it fires *during*
    // that block — getservercert must already be in flight for Sunshine to
    // attach the PIN to the pending pairing request.
    auto* rest = new SunshineRestClient(&computers);
    QTimer::singleShot(800, rest, [rest, pin, user, pass, restPort]() {
        rest->sendPin(pin, user, pass, QStringLiteral("MoonlightWeb"), restPort);
    });

    auto [submitStatus, submitResult] = computers.handleSubmitPin(uuid);
    const QString state = submitResult.value(QStringLiteral("status")).toString();
    Logger::info(QStringLiteral("Provisioning: local Sunshine pairing -> %1 (%2)")
                     .arg(state, submitResult.value(QStringLiteral("message")).toString()));

    NvComputer* paired = computers.getHost(uuid);
    return (paired && paired->pairState == NvComputer::PS_PAIRED)
           || state == QLatin1String("paired");
}

bool applyOnce(const QString& exeDir, AppSettings& settings, ComputerManager& computers)
{
    const QString path = exeDir + QStringLiteral("/provisioning.json");
    QFile file(path);
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::warning(QStringLiteral("Provisioning: cannot read %1").arg(path));
        return false;
    }

    QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    Logger::info(QStringLiteral("Provisioning: applying %1").arg(path));

    const bool internet = obj.value(QStringLiteral("internet_access_authorized")).toBool();
    const QJsonObject sun = obj.value(QStringLiteral("sunshine")).toObject();
    const bool autoPair = sun.value(QStringLiteral("auto_pair")).toBool();

    // Seed the live checklist up front so the installer renders the full task
    // list immediately. The A-record completes asynchronously (see main.cpp);
    // pairing resolves synchronously below.
    setStepStatus(QStringLiteral("pairing"), autoPair ? QStringLiteral("running")
                                                      : QStringLiteral("skipped"));
    setStepStatus(QStringLiteral("arecord"), internet ? QStringLiteral("running")
                                                       : QStringLiteral("skipped"));

    // Internet Access: just flip the persisted flag; main()'s existing
    // auto-start path brings the InternetAccessManager up after this returns.
    if (internet) {
        settings.setInternetAccessEnabled(true);
        Logger::info(QStringLiteral("Provisioning: Internet Access authorized"));
    }

    if (autoPair) {
        const QString user = sun.value(QStringLiteral("username")).toString(QStringLiteral("admin"));
        const QString pass = sun.value(QStringLiteral("password")).toString(QStringLiteral("admin"));
        const bool ok = pairLocalSunshine(computers, user, pass);
        setStepStatus(QStringLiteral("pairing"),
                      ok ? QStringLiteral("done") : QStringLiteral("failed"));
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
    return true;
}

} // namespace Provisioning
