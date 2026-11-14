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
#ifdef Q_OS_WIN
namespace mw {
class OwnedChild;
}
#endif

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
 *            elevates with NO UAC prompt. Its action is our own exe under
 *            {app} with --update-apply (UpdateApply.h): it copies what we
 *            staged under %LocalAppData% somewhere the user cannot write and
 *            runs it only once it matches GitHub's digest (and our signer).
 *            Fallback: launch the installer directly → UAC on the host desktop.
 *   Linux    AppImage → replace the file in place, no root at all. Running as
 *            root (systemd system unit) → dpkg/rpm directly. A .deb/.rpm of
 *            the production edition → its root helper upgrades it from our
 *            signed repository, through a polkit action the package grants
 *            to the active session without a password.
 *            Fallback: pkexec → polkit prompt on the host desktop.
 *   macOS    bundle owned by the user (the .pkg's postinstall hands it over)
 *            → unpack the .pkg, check the app's signature against ours, copy
 *            it over the bundle: no root at all. Root → `installer -pkg`.
 *            Fallback (a root-owned bundle, i.e. any install made before the
 *            hand-over): osascript "with administrator privileges" → password
 *            prompt on the host desktop.
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

    // Where the downloaded installer is staged. On Windows the elevated
    // --update-apply helper (UpdateApply.h) picks it up from this same path,
    // copies it somewhere the user cannot write and verifies the copy.
    static QString stagingDir();
    static QString stagedFileName(const QString& assetName);

private:
    enum class State
    {
        Idle,
        Downloading,
        Installing,
        Restarting,
        Failed
    };

    // Stage and fetch the asset UpdateChecker currently resolves, from whatever
    // its cache holds right now. Split out of start() so a download refused by
    // the checks below can be replayed against re-fetched metadata without
    // asking the user to click again. Empty on success, else the error.
    QString beginDownload();
    void onDownloadFinished();
    // Empty when the staged file matches the digest GitHub published for it (or
    // when there is no digest to check against); the error message otherwise.
    QString verifyDigest();
    // A download that does not match what the checker promised is, far more
    // often than a broken transfer, a release that was re-published since the
    // last check. Refetch and replay the download once; true when that is under
    // way (the caller must return), false when it is not worth trying again.
    bool retryOnStaleMetadata(const QString& reason);
    // Installing state, pseudo-progress, then runInstaller().
    void startInstallPhase();
    void runInstaller();
    void onInstallerFinished(int exitCode, bool crashed);
    void armWatchdog();
#ifndef Q_OS_WIN
    // Start the detached install → kill us → relaunch sequence. False (and the
    // update failed) when even that could not start.
    bool launchDetached(const QString& install);
#endif
#ifdef Q_OS_MACOS
    // The "bundle" path: unpack the .pkg, check the app it carries against our
    // own designated requirement, then copy it over the user-owned bundle.
    void prepareBundle();
#endif
#ifdef Q_OS_WIN
    // Run the installer from a transient LocalSystem task so it is NOT a child
    // of this process. Empty on success, else the error to report.
    QString startViaSystemTask(const QString& program, const QStringList& args);
#endif
    // Where the installer is told to write its own log — the only account of what
    // it did once message boxes are suppressed.
    static QString installerLogPath();
    // `dropStaged` false keeps the downloaded installer on disk — used when it
    // may still be picked up by a prompt waiting on the host desktop.
    void fail(const QString& message, bool dropStaged = true);

    // Preferred elevation path for this machine ("scheduled-task", "service",
    // "appimage", "bundle", "repo", "root", "direct", "pkexec", "osascript"). Empty when the
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
    // How long the stale-metadata retry waits for a fresh check before giving up
    // and reporting the original mismatch. Generous — the request itself times
    // out at 8 s — but bounded: the UI is sitting on "downloading" meanwhile.
    static constexpr int kRecheckTimeoutMs = 20000;

    UpdateChecker* m_checker;
    QNetworkAccessManager* m_nam;
    QNetworkReply* m_reply = nullptr;
    QFile* m_file = nullptr;
    QTimer* m_installTick = nullptr;       // pseudo-progress while the installer runs
    int m_installElapsedMs = 0;            // drives the install easing curve
    QTimer* m_watchdog = nullptr;          // gives up when the installer never lands
    QProcess* m_installer = nullptr;       // Windows installer / macOS unpack: exit code
    QTimer* m_recheckGuard = nullptr;      // bounds the wait on a forced check
    QMetaObject::Connection m_recheckWait; // that check's one-shot subscription
    bool m_recheckDone = false;            // one retry per start(), never a loop
#ifdef Q_OS_WIN
    // The Windows installer, or the schtasks that starts its task: a child this
    // process never ends, not even by quitting (OwnedChild.h), since quitting
    // is what that installer asks of it.
    mw::OwnedChild* m_ownedInstaller = nullptr;
#endif

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
