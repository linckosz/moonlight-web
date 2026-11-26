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

#include "backend/GamepadDriver.h"

#include "common/ChildStdio.h"
#include "common/Logger.h"
#include "common/PrivilegedPlace.h"
#include "common/WinSystemPath.h"
#include "network/AddressRace.h"

#include "mw/native/NativeHost.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>

#include <memory>
#include <QUrl>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace GamepadDriver {
namespace {

// A WiX bundle's unattended form. Never /forcerestart: a driver install can
// legitimately want a reboot, and rebooting the machine someone is streaming
// from would be a spectacular way to answer "install the gamepad driver".
const QStringList kSilentArgs = {QStringLiteral("/quiet"), QStringLiteral("/norestart")};

// A stalled download must not hold the attempt open forever; the bundle itself
// is ~4 MB, so an idle minute is already generous.
constexpr int kTransferTimeoutMs = 60 * 1000;
constexpr int kInstallTimeoutMs = 10 * 60 * 1000;

// Reached only from the Qt main thread: both routes run there, and the install
// chain below is event-driven rather than threaded precisely so that stays true.
bool g_probed = false;
Status g_status;
bool g_installing = false;

/// Where the downloaded installer is staged: a directory of this process's
/// own under the install directory's host-data, which only SYSTEM and
/// Administrators may read or write (common/PrivilegedPlace.h). It is an
/// executable this process is about to run with its own rights, and the
/// install only ever runs in a process that has them (canInstall()). Not
/// under System32 either, where the 32-bit WiX bundle cannot find itself (see
/// isUnderSystem32). Empty when no such directory could be had.
QString stagingDir()
{
#ifdef Q_OS_WIN
    return mw::place::privateDir(QStringLiteral("driver"));
#else
    return QDir::tempPath() + QStringLiteral("/MoonlightWeb-driver");
#endif
}

QString stagedPath()
{
    return stagingDir() + QStringLiteral("/ViGEmBus-setup.exe");
}

using DoneFn = std::function<void(Result, QString)>;

/// End one attempt: drop the staged executable, release the lock, answer.
void finish(QNetworkAccessManager* nam, Result result, const QString& error, const DoneFn& done)
{
    QFile::remove(stagedPath());
    g_installing = false;
    if (nam) nam->deleteLater();

    switch (result) {
    case Result::Installed: Logger::info(QStringLiteral("[gamepad] ViGEmBus installed")); break;
    case Result::RestartRequired:
        Logger::info(QStringLiteral("[gamepad] ViGEmBus installed — a restart will activate it"));
        break;
    case Result::Failed:
        Logger::warning(QStringLiteral("[gamepad] ViGEmBus install failed: %1").arg(error));
        break;
    }
    done(result, error);
}

/// Run the staged bundle, then ask the driver again.
void runInstaller(QNetworkAccessManager* nam, const DoneFn& done)
{
    QProcess* proc = new QProcess();
    QTimer* deadline = new QTimer(proc);
    deadline->setSingleShot(true);

    // The file that is run is the one that was verified: held from here until
    // the installer is done, so nothing writes to it or takes its place, and
    // hashed again through that hold.
    auto held = std::make_shared<mw::place::HeldFile>(stagedPath());
    if (!held->isHeld() || held->sha256() != downloadSha256()) {
        const QString why =
            held->isHeld() ? QStringLiteral("it changed after it was written") : held->error();
        held->release();
        proc->deleteLater();
        finish(nam, Result::Failed,
               QStringLiteral("The staged driver installer cannot be run (%1)").arg(why), done);
        return;
    }
    // Let go of it when the installer ends, whichever way: connected first, so
    // it is done before the slots below delete the file.
    QObject::connect(proc, &QProcess::finished, proc, [held]() { held->release(); });
    QObject::connect(proc, &QProcess::errorOccurred, proc, [held](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) held->release();
    });

    QObject::connect(deadline, &QTimer::timeout, proc, [proc]() {
        Logger::warning(
            QStringLiteral("[gamepad] the ViGEmBus installer did not finish — killing it"));
        proc->kill();
    });

    QObject::connect(
        proc, &QProcess::finished, proc,
        [proc, nam, done](int code, QProcess::ExitStatus exitStatus) {
            proc->deleteLater();

            if (exitStatus != QProcess::NormalExit) {
                finish(nam, Result::Failed, QStringLiteral("The driver installer did not finish"),
                       done);
                return;
            }
            // 3010 is ERROR_SUCCESS_REBOOT_REQUIRED: installed, but the bus does
            // not appear until the machine restarts. Not a failure — the probe
            // below is what decides what to say about it.
            if (code != 0 && code != 3010) {
                finish(nam, Result::Failed,
                       QStringLiteral("The driver installer exited with code %1").arg(code), done);
                return;
            }

            const Status after = refresh();
            // Installed, yet the bus still will not open: the reboot case. Its
            // own answer, not a failure — see Result.
            finish(nam, after.present ? Result::Installed : Result::RestartRequired, QString(),
                   done);
        });

    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [proc, nam, done](QProcess::ProcessError err) {
                         if (err != QProcess::FailedToStart) return; // finished() covers the rest
                         proc->deleteLater();
                         finish(nam, Result::Failed,
                                QStringLiteral("The driver installer could not be started"), done);
                     });

    Logger::info(QStringLiteral("[gamepad] running the ViGEmBus installer silently"));
    deadline->start(kInstallTimeoutMs);
    mw::passOnlyStdio(*proc);
    proc->start(stagedPath(), kSilentArgs);
}

} // namespace

Status refresh()
{
    const mw::native::VirtualGamepad probe = mw::native::NativeHost::probeVirtualGamepad();
    g_status.supported = probe.supported;
    g_status.present = probe.present;
    g_status.diagnostic = QString::fromStdString(probe.diagnostic);
    g_probed = true;
    return g_status;
}

Status status()
{
    // Re-probed while the bus is missing, cached once it is there. That
    // asymmetry is deliberate: "absent" is the state a user can change behind
    // our back — by installing the driver by hand from the link the notice
    // offers — and the notice has to disappear on its own when they do. Asking
    // again costs a device open that fails immediately.
    //
    // "Present" needs no such refresh: nobody uninstalls a driver mid-session,
    // and if they did, the cost is one session without a gamepad, which is the
    // clean degradation this feature exists inside of.
    if (!g_probed || !g_status.present) return refresh();
    return g_status;
}

bool canInstall()
{
#ifdef Q_OS_WIN
    // Asked of the token rather than assumed from MW_SERVICE: an instance
    // started elevated by hand can install perfectly well, and a service that
    // somehow is not elevated must not be told it can.
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
#else
    return false;
#endif
}

void install(std::function<void(Result, QString)> done)
{
    if (g_installing) {
        done(Result::Failed, QStringLiteral("A driver install is already running"));
        return;
    }
    if (!canInstall()) {
        done(Result::Failed,
             QStringLiteral("This instance is not elevated — install the driver from %1")
                 .arg(downloadUrl()));
        return;
    }
    const QString staging = stagingDir();
    if (staging.isEmpty() || !QDir().mkpath(staging)) {
        done(Result::Failed, QStringLiteral("Cannot create the driver staging directory"));
        return;
    }
    // A leftover from an interrupted attempt would be run as-is otherwise.
    QFile::remove(stagedPath());

    g_installing = true;

    QNetworkAccessManager* nam = new QNetworkAccessManager();
    QNetworkRequest req{QUrl(downloadUrl())};
    req.setRawHeader("User-Agent", "MoonlightWeb");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    AddressRace::enableOn(req);
    req.setTransferTimeout(kTransferTimeoutMs);

    Logger::info(QStringLiteral("[gamepad] downloading %1").arg(downloadUrl()));
    QNetworkReply* reply = nam->get(req);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, nam, done]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            finish(nam, Result::Failed,
                   QStringLiteral("Download failed: %1").arg(reply->errorString()), done);
            return;
        }

        const QByteArray payload = reply->readAll();
        // Verified BEFORE it is written anywhere, let alone run: this executes
        // as SYSTEM. A mismatch is a mismatch — a replaced asset, a proxy that
        // rewrote the download, an HTML error page saved as an installer — and
        // none of those is something to "try anyway".
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        if (digest != downloadSha256()) {
            finish(nam, Result::Failed,
                   QStringLiteral("The downloaded driver installer does not match the published "
                                  "one (%1 bytes, sha256 %2…) — install it manually from %3")
                       .arg(payload.size())
                       .arg(digest.left(12))
                       .arg(downloadUrl()),
                   done);
            return;
        }

        // A new file: nothing already under that name is written to.
        QFile file(stagedPath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(payload) != payload.size()) {
            file.close();
            finish(nam, Result::Failed,
                   QStringLiteral("Cannot write to the driver staging directory"), done);
            return;
        }
        file.close();

        runInstaller(nam, done);
    });
}

} // namespace GamepadDriver
