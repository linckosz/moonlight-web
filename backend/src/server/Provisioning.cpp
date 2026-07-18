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
#include <QThread>
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

// Publish a top-level informational key (e.g. the resolved admin URL) in the
// same status file; the installer reads it to open the right page post-install.
void setInfo(const QString& key, const QString& value)
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
    root[key] = value;

    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

// Pair the local Sunshine over the GameStream protocol while feeding the PIN
// through Sunshine's REST API (no manual entry in Sunshine's web UI).
bool pairSunshine(ComputerManager& computers, const QString& user, const QString& pass)
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

    // Sunshine's REST config API ("/api/pin", HTTP basic-auth web UI) listens on
    // the base port + 1 (47990 by default). This is NOT the GameStream HTTPS port
    // kept in activeHttpsPort, which does not serve /api/pin and drops the
    // connection ("Connection closed"). Derive it from the host's base HTTP port.
    quint16 basePort = MW_HTTP_PORT;
    if (host && host->manualAddress.port() > 0) basePort = host->manualAddress.port();
    const quint16 restPort = basePort + 1;
    auto* rest = new SunshineRestClient(&computers);

    // Dual-stream seamless switching needs Sunshine to accept at least TWO
    // concurrent sessions ("Maximum Connected Clients", config key `channels`,
    // default 1). Raise it to 2 when lower; leave any higher value untouched.
    // Best-effort — a refusal only means the runtime probe reports
    // dual_unavailable and streaming falls back to the legacy relaunch.
    rest->ensureMinChannels(2, user, pass, restPort);

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

    // Pairing stage 1 (getservercert) stays in flight until Sunshine receives
    // the PIN. Schedule the REST push so it fires *after* the chain has started
    // and getservercert is already in flight, letting Sunshine attach the PIN to
    // the pending request.
    QTimer::singleShot(800, rest, [rest, pin, user, pass, restPort]() {
        rest->sendPin(pin, user, pass, QStringLiteral("MoonlightWeb"), restPort);
    });

    // Drive the (asynchronous) pairing chain to completion under a local event
    // loop. Safe here: provisioning runs once at startup, before the main event
    // loop and outside the reentrant HTTP request path.
    const bool paired = computers.pairHostBlocking(uuid, 65000);
    Logger::info(QStringLiteral("Provisioning: local Sunshine pairing -> %1")
                     .arg(paired ? QStringLiteral("paired") : QStringLiteral("failed")));
    return paired;
}

bool applyOnce(const QString& exeDir, AppSettings& settings, ComputerManager& computers)
{
    // The installer drops provisioning.json next to the executable (Windows +
    // Linux). On macOS the .pkg postinstall runs as root and cannot write inside
    // the signed app bundle (exeDir = MoonlightWeb.app/Contents/MacOS), so it
    // writes into the per-user data dir instead — fall back to it. The consumed
    // marker and removal happen next to whichever file we actually found.
    QString dir = exeDir;
    QString path = dir + QStringLiteral("/provisioning.json");
    if (!QFile::exists(path)) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        path = dir + QStringLiteral("/provisioning.json");
    }
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
    setStepStatus(QStringLiteral("pairing"),
                  autoPair ? QStringLiteral("running") : QStringLiteral("skipped"));
    setStepStatus(QStringLiteral("arecord"),
                  internet ? QStringLiteral("running") : QStringLiteral("skipped"));

    // Internet Access: just flip the persisted flag; main()'s existing
    // auto-start path brings the InternetAccessManager up after this returns.
    if (internet) {
        // Legal traceability: the installer records the exact agreement text the
        // user read when ticking the Internet checkbox (localized).
        settings.setInternetConsent(obj.value(QStringLiteral("consent_message")).toString(),
                                    QStringLiteral("installer"));
        settings.setInternetAccessEnabled(true);
        Logger::info(QStringLiteral("Provisioning: Internet Access authorized"));
    }

    if (autoPair) {
        const QString user =
            sun.value(QStringLiteral("username")).toString(QStringLiteral("admin"));
        const QString pass =
            sun.value(QStringLiteral("password")).toString(QStringLiteral("admin"));
        // The installer typically launched Sunshine moments before this app (the
        // macOS postinstall starts both back-to-back): retry a couple of times so
        // a Sunshine still opening its GameStream port doesn't fail the step.
        bool ok = pairSunshine(computers, user, pass);
        for (int attempt = 0; !ok && attempt < 2; ++attempt) {
            QThread::sleep(3);
            ok = pairSunshine(computers, user, pass);
        }
        setStepStatus(QStringLiteral("pairing"),
                      ok ? QStringLiteral("done") : QStringLiteral("failed"));
    }

    // The installer owns first-run provisioning: mark setup done so the app
    // opens the normal admin page, not the in-app setup wizard (which stays
    // available as a fallback if Sunshine turns out to be missing).
    settings.setSetupCompleted(true);

    // Consume: rewrite without the password, then remove the original so the
    // plaintext credential never lingers and provisioning is not replayed.
    QJsonObject consumedSun = sun;
    consumedSun.remove(QStringLiteral("password"));
    if (!consumedSun.isEmpty()) obj[QStringLiteral("sunshine")] = consumedSun;
    QFile consumed(dir + QStringLiteral("/provisioning.consumed.json"));
    if (consumed.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        consumed.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        consumed.close();
    }
    QFile::remove(path);
    return true;
}

} // namespace Provisioning
