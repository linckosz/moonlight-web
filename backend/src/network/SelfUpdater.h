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

#include <QJsonObject>
#include <QObject>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;
class UpdateChecker;

/**
 * Applies a MoonlightWeb update on the host, driven from the web app.
 *
 * UpdateChecker only *detects* a newer release; this downloads the installer it
 * resolved for this OS/arch and runs it unattended — no wizard, no question, the
 * existing configuration is kept as-is (see the installer's update mode).
 *
 * ── Why elevation is the hard part ────────────────────────────────────────────
 * The trigger comes from a browser that may be on the other side of the planet,
 * so any elevation prompt that lands on the host's physical desktop is a dead
 * end. Each platform therefore has a preferred silent path and a degraded one:
 *
 *   Windows  scheduled task "MoonlightWeb Update" (registered elevated by the
 *            installer, RunLevel=HighestAvailable, no trigger) → `schtasks /Run`
 *            elevates with NO UAC prompt. Its action points at a fixed staging
 *            path under %LocalAppData% (per-user, so no other local account can
 *            plant an executable that we would then run elevated).
 *            Fallback: launch the installer directly → UAC on the host desktop.
 *   Linux    AppImage → replace the file in place, no root at all. Running as
 *            root (systemd system unit) → dpkg/rpm directly.
 *            Fallback: pkexec → polkit prompt on the host desktop.
 *   macOS    root → `installer -pkg`. Fallback: osascript "with administrator
 *            privileges" → password prompt on the host desktop.
 *
 * capabilityJson() reports which path applies so the UI can warn ("a
 * confirmation is required on the host PC") instead of hanging on a prompt
 * nobody can see.
 *
 * ── Lifetime ─────────────────────────────────────────────────────────────────
 * The installer replaces (and on Windows kills) this very process, so progress
 * reporting is one-way and stops mid-install by design: the client polls
 * statusJson() until the connection drops, then waits for the new build to
 * answer /api/health. Every platform path ends with a relaunch of the app; the
 * single-instance lock makes a duplicate relaunch a no-op.
 */
class SelfUpdater : public QObject
{
    Q_OBJECT
public:
    explicit SelfUpdater(UpdateChecker* checker, QObject* parent = nullptr);

    // { supported, method, requires_host_confirmation } — static platform
    // capability, independent of whether an update is actually available.
    QJsonObject capabilityJson() const;

    // { state, percent, version, requires_host_confirmation, error }
    // state ∈ idle | downloading | installing | restarting | failed
    QJsonObject statusJson() const;

    // Download + apply the update UpdateChecker resolved. Returns an empty
    // string once the download is under way, or a human-readable error.
    QString start();

private:
    enum class State
    {
        Idle,
        Downloading,
        Installing,
        Restarting,
        Failed
    };

    void onDownloadFinished();
    // Empty when the staged file matches the digest GitHub published for it (or
    // when there is no digest to check against); the error message otherwise.
    QString verifyDigest();
    void runInstaller();
    void onInstallerFinished(int exitCode, bool crashed);
    // `dropStaged` false keeps the downloaded installer on disk — used when it
    // may still be picked up by a prompt waiting on the host desktop.
    void fail(const QString& message, bool dropStaged = true);

    // Where the downloaded installer is staged. On Windows this MUST match the
    // path baked into the elevated scheduled task's action (see moonlightweb.iss
    // RegisterUpdateTask) or the task runs nothing.
    static QString stagingDir();
    static QString stagedFileName(const QString& assetName);

    // Preferred elevation path for this machine ("scheduled-task", "service",
    // "appimage", "root", "direct", "pkexec", "osascript"). Empty when the
    // platform has no unattended path at all.
    static QString elevationMethod();
    static bool methodNeedsHostConfirmation(const QString& method);

    // Budget split, tuned on *elapsed time* rather than bytes: the download is
    // a couple of seconds on any usable link while the install + relaunch is
    // 15-30 s, so giving the download most of the bar made it jump straight to
    // ~70 % and then sit there. Keep it small; the install owns the rest.
    static constexpr int kDownloadShare = 25;
    // The install has no readable progress, so it eases towards a ceiling it
    // never reaches — "still working, slowing down" is the only honest shape.
    // The client keeps interpolating past it once we are killed.
    static constexpr int kInstallCeiling = 90;
    static constexpr double kInstallTauSec = 14.0;
    static constexpr int kInstallTickMs = 250;

    UpdateChecker* m_checker;
    QNetworkAccessManager* m_nam;
    QNetworkReply* m_reply = nullptr;
    QFile* m_file = nullptr;
    QTimer* m_installTick = nullptr; // pseudo-progress while the installer runs
    int m_installElapsedMs = 0;      // drives the install easing curve
    QTimer* m_watchdog = nullptr;    // gives up when the installer never lands
    QProcess* m_installer = nullptr; // Windows: kept to read its exit code

    State m_state = State::Idle;
    int m_percent = 0;
    QString m_error;
    QString m_target;  // version being installed
    QString m_pkgPath; // staged installer on disk
    QString m_method;  // elevation path chosen for this run
    // What GitHub says the asset is, captured when the download starts so a
    // release edited mid-flight cannot move the goalposts.
    qint64 m_expectedSize = 0;
    QString m_expectedDigest;
};
