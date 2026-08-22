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

QString stepStatus(const QString& step)
{
    QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/provisioning.status.json"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    return root.value(QStringLiteral("steps")).toObject().value(step).toString();
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
    // Wait until the freshly-launched Sunshine is actually ready to pair BEFORE
    // emitting the GameStream getservercert — the request that pops Sunshine's
    // "Pairing incoming" slider. Two conditions must hold, and both are probed
    // without raising a slider: its GameStream port must answer (handleAddManualHost
    // brings the host CS_ONLINE) and its REST API must accept our credentials (so
    // the PIN push below can actually be delivered). Gating readiness here — rather
    // than retrying the whole pairing — is what keeps a single slider on screen: a
    // premature getservercert whose PIN never lands leaves an orphaned prompt, and
    // a retry would raise a second one.
    QString uuid;
    NvComputer* host = nullptr;
    quint16 restPort = MW_HTTP_PORT + 1;

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 20000;
    bool ready = false;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        auto [addStatus, addResult] = computers.handleAddManualHost(QStringLiteral("127.0.0.1"));
        if (addStatus != 200) {
            Logger::info(QStringLiteral("Provisioning: Sunshine GameStream port not ready yet — "
                                        "waiting"));
            QThread::sleep(1);
            continue;
        }

        const QJsonArray hosts = addResult.value(QStringLiteral("hosts")).toArray();
        if (hosts.isEmpty()) {
            QThread::sleep(1);
            continue;
        }
        uuid = hosts.first().toObject().value(QStringLiteral("uuid")).toString();
        if (uuid.isEmpty()) {
            QThread::sleep(1);
            continue;
        }

        host = computers.getHost(uuid);
        if (host && host->pairState == NvComputer::PS_PAIRED) {
            Logger::info(QStringLiteral("Provisioning: local Sunshine already paired"));
            return true;
        }

        // Sunshine's REST config API ("/api/pin", HTTP basic-auth web UI) listens
        // on the base port + 1 (47990 by default). This is NOT the GameStream
        // HTTPS port kept in activeHttpsPort, which does not serve /api/pin and
        // drops the connection ("Connection closed"). Derive it from the host's
        // base HTTP port.
        quint16 basePort = MW_HTTP_PORT;
        if (host && host->manualAddress.port() > 0) basePort = host->manualAddress.port();
        restPort = basePort + 1;

        // No slider: this is a plain GET /api/apps, not a GameStream getservercert.
        SunshineRestClient probe;
        const auto cred = probe.checkCredentials(user, pass, restPort);
        if (cred.outcome == SunshineRestClient::CredentialCheck::Accepted) {
            ready = true;
            break;
        }
        if (cred.outcome == SunshineRestClient::CredentialCheck::Rejected) {
            // Bad credentials: no point popping a slider that could never complete.
            Logger::warning(QStringLiteral("Provisioning: Sunshine refused the provisioned "
                                           "credentials — cannot auto-pair"));
            return false;
        }
        // Unreachable: REST API is still coming up — retry.
        QThread::sleep(1);
    }

    if (!ready || uuid.isEmpty()) {
        Logger::warning(QStringLiteral("Provisioning: Sunshine did not become ready to pair"));
        return false;
    }

    // Sunshine is up and our credentials work: emitting getservercert now yields
    // exactly one slider, and the scheduled PIN push is guaranteed deliverable.
    auto [startStatus, startResult] = computers.handleStartPairing(uuid);
    if (startResult.value(QStringLiteral("status")).toString() != QLatin1String("initiated")) {
        Logger::warning(QStringLiteral("Provisioning: pairing could not start: %1")
                            .arg(startResult.value(QStringLiteral("message")).toString()));
        return false;
    }
    const QString pin = startResult.value(QStringLiteral("pin")).toString();

    auto* rest = new SunshineRestClient(&computers);

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

    // Nothing else to configure on Sunshine: its old "Maximum Connected Clients"
    // setting (config key `channels`) is gone from current Sunshine, which tracks
    // sessions without a numeric cap. Dual-stream capability is settled at
    // runtime by the standby launch itself — a host that refuses the second
    // session answers dual_unavailable and streaming falls back to the legacy
    // relaunch.
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
        // user read when ticking the Internet checkbox (localized). Only on the
        // first authorization — an upgrade skips the consent page (the checkbox
        // is pre-answered from settings.json), so what it forwards here is not
        // a consent the user just gave, and must not overwrite the one they did.
        if (settings.internetConsent().isEmpty()) {
            settings.setInternetConsent(obj.value(QStringLiteral("consent_message")).toString(),
                                        QStringLiteral("installer"),
                                        QStringLiteral("rendezvous"));
        }
        settings.setInternetAccessEnabled(true);
        Logger::info(QStringLiteral("Provisioning: Internet Access authorized"));
    }

    if (autoPair) {
        const QString user =
            sun.value(QStringLiteral("username")).toString(QStringLiteral("admin"));
        const QString pass =
            sun.value(QStringLiteral("password")).toString(QStringLiteral("admin"));
        // The installer typically launched Sunshine moments before this app (the
        // macOS postinstall starts both back-to-back). pairSunshine() now waits for
        // Sunshine's GameStream and REST APIs to come up before emitting the
        // getservercert, so a single call suffices — retrying here would only risk a
        // second "Pairing incoming" slider.
        const bool ok = pairSunshine(computers, user, pass);
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
