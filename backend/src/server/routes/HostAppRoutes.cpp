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

#include "server/routes/HostAppRoutes.h"

#include "backend/VirtualDisplay.h"
#include "backend/streambackend/NativeHostBackend.h"
#include "common/Logger.h"
#include "server/AppSettings.h"
#include "server/HostAppImage.h"
#include "server/HttpServer.h"
#include "server/LauncherDetect.h"
#include "server/RestRouter.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

HttpResponse adminOnly()
{
    return HttpResponse::error(403, "The host's apps can only be changed by its administrator");
}

/// The image of a launcher's card: its program's icon on a dark card. Its
/// hash, or empty when the program has no icon to read.
QString storeLauncherImage(AppSettings& appSettings, const QString& id, const QString& iconPath)
{
    const QImage icon = HostAppImage::iconOf(iconPath);
    if (icon.isNull()) return QString();
    QString why;
    const QString hash = HostAppImage::store(appSettings.hostAppImagesDir(), id,
                                             HostAppImage::cardFromIcon(icon), &why);
    if (hash.isEmpty())
        Logger::warning(QStringLiteral("[HostApp] no image for %1: %2").arg(iconPath, why));
    else
        appSettings.setHostAppImage(id, hash);
    return hash;
}

const LauncherDetect::Launcher* launcherOf(const QVector<LauncherDetect::Launcher>& found,
                                           const QString& source)
{
    for (const LauncherDetect::Launcher& launcher : found)
        if (launcher.source == source) return &launcher;
    return nullptr;
}

QJsonObject listJson(AppSettings& appSettings)
{
    QJsonObject obj;
    // Wherever the native host can stream: the apps open on its screen.
    obj["supported"] = NativeHostBackend::isEnabled() &&
                       (NativeHostBackend::isAvailable() || VirtualDisplay::probe().installed);
    obj["max"] = AppSettings::kHostAppsMax;
    obj["screen"] = NativeHostBackend::toString(NativeHostBackend::hostAppScreen());
    const QJsonArray apps = appSettings.hostApps();
    obj["apps"] = apps;
    // The launchers found on this PC whose card the owner removed: the page
    // offers them back.
    const QStringList dismissed = appSettings.hostAppsDismissed();
    QJsonArray detected;
    for (const LauncherDetect::Launcher& launcher : LauncherDetect::detect()) {
        bool listed = false;
        for (const QJsonValue& v : apps)
            if (v.toObject().value("source").toString() == launcher.source) listed = true;
        detected.append(QJsonObject{{"source", launcher.source},
                                    {"name", launcher.name},
                                    {"listed", listed},
                                    {"dismissed", dismissed.contains(launcher.source)}});
    }
    obj["detected"] = detected;
    return obj;
}

} // namespace

void seedDetectedLaunchers(AppSettings& appSettings)
{
    const QVector<LauncherDetect::Launcher> found = LauncherDetect::detect();
    QJsonArray added;
    if (!appSettings.seedHostApps(LauncherDetect::toJson(found),
                                  QCoreApplication::applicationVersion(), &added))
        return;
    for (const QJsonValue& v : added) {
        const QJsonObject entry = v.toObject();
        const LauncherDetect::Launcher* launcher =
            launcherOf(found, entry.value("source").toString());
        if (launcher)
            storeLauncherImage(appSettings, entry.value("id").toString(), launcher->iconPath);
        Logger::info(QStringLiteral("[HostApp] found on this PC: %1 — its card is on the host")
                         .arg(entry.value("name").toString()));
    }
}

void registerHostAppRoutes(HttpServer& server, AppSettings& appSettings)
{
    server.router()->get("/api/host-apps", [&appSettings](const HttpRequest& req) {
        if (!req.isLocal) return adminOnly();
        return HttpResponse::json(listJson(appSettings));
    });

    server.router()->post("/api/host-apps", [&appSettings](const HttpRequest& req) {
        if (!req.isLocal) return adminOnly();
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        if (!body.value("apps").isArray()) return HttpResponse::error(400, "apps must be a list");
        QString why;
        QStringList removed;
        if (!appSettings.setHostApps(body.value("apps").toArray(), &why, &removed))
            return HttpResponse::error(400, why);
        for (const QString& id : removed)
            HostAppImage::remove(appSettings.hostAppImagesDir(), id);
        return HttpResponse::json(listJson(appSettings));
    });

    server.router()->post("/api/host-apps/restore", [&appSettings](const HttpRequest& req) {
        if (!req.isLocal) return adminOnly();
        const QString source =
            QJsonDocument::fromJson(req.body).object().value("source").toString();
        const QVector<LauncherDetect::Launcher> found = LauncherDetect::detect();
        const LauncherDetect::Launcher* launcher = launcherOf(found, source);
        if (!launcher) return HttpResponse::error(404, "That launcher is not on this PC");
        QString why;
        const QJsonObject entry =
            appSettings.restoreHostApp(LauncherDetect::toJson(*launcher), &why);
        if (entry.isEmpty()) return HttpResponse::error(400, why);
        storeLauncherImage(appSettings, entry.value("id").toString(), launcher->iconPath);
        return HttpResponse::json(listJson(appSettings));
    });

    server.router()->post("/api/host-apps/:id/image", [&appSettings](const HttpRequest& req) {
        if (!req.isLocal) return adminOnly();
        const QString id = req.pathParams.value("id");
        bool known = false;
        for (const QJsonValue& v : appSettings.hostApps())
            if (v.toObject().value("id").toString() == id) known = true;
        if (!known) return HttpResponse::error(404, "No such app");
        const QByteArray png = QByteArray::fromBase64(
            QJsonDocument::fromJson(req.body).object().value("png").toString().toLatin1());
        QImage image;
        QString why;
        if (!HostAppImage::decodeUpload(png, &image, &why)) return HttpResponse::error(400, why);
        const QString hash = HostAppImage::store(appSettings.hostAppImagesDir(), id,
                                                 HostAppImage::cardFrom(image), &why);
        if (hash.isEmpty()) return HttpResponse::error(500, why);
        appSettings.setHostAppImage(id, hash);
        return HttpResponse::json(listJson(appSettings));
    });

    server.router()->del("/api/host-apps/:id/image", [&appSettings](const HttpRequest& req) {
        if (!req.isLocal) return adminOnly();
        const QString id = req.pathParams.value("id");
        if (!appSettings.setHostAppImage(id, QString()))
            return HttpResponse::error(404, "No such app");
        HostAppImage::remove(appSettings.hostAppImagesDir(), id);
        return HttpResponse::json(listJson(appSettings));
    });

    // The GPU that draws the virtual display, chosen on its card on a PC with
    // several (device.gpuChoice). Under the host's own path, so a booth
    // (--noadmin) refuses it like every host route it does not play on; the
    // administrator's alone, as the host's apps are. "" gives the choice back
    // to the driver. The next activation follows (VirtualDisplayJob).
    server.router()->post("/api/hosts/:id/virtual-display-gpu", [&appSettings](
                                                                    const HttpRequest& req) {
        if (!req.isLocal)
            return HttpResponse::error(
                403, "Only the host's administrator chooses the virtual display's GPU");
        if (req.pathParams.value("id") != NativeHostBackend::hostUuid())
            return HttpResponse::error(404, "Only this PC's own host has a virtual display");
        const QJsonValue gpu = QJsonDocument::fromJson(req.body).object().value("gpu");
        if (!gpu.isString()) return HttpResponse::error(400, "gpu must be a string");
        if (!NativeHostBackend::virtualDisplayGpuChoosable(gpu.toString()))
            return HttpResponse::error(409, "That GPU cannot be chosen for the virtual display");
        appSettings.setVirtualDisplayGpu(gpu.toString());
        Logger::info(QStringLiteral("[HostApp] virtual display drawn by %1 from its next start")
                         .arg(gpu.toString().isEmpty() ? QStringLiteral("the driver's choice")
                                                       : gpu.toString()));
        return HttpResponse::json(QJsonObject{{"gpu", appSettings.virtualDisplayGpu()}});
    });
}
