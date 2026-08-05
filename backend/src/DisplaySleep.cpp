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

#include "DisplaySleep.h"

#include "common/DesktopSession.h"
#include "common/Logger.h"

#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QtGlobal>

namespace DisplaySleep {

#if defined(Q_OS_LINUX)

namespace {

// The two GNOME keys that decide whether an idle machine keeps scanning out a
// picture. `idle-delay` (seconds, 0 = never) blanks the screen;
// `sleep-inactive-ac-type` ('nothing' = never) suspends the whole box. Both must
// be neutralised: blanking alone already breaks capture, and a suspend obviously
// does too. Battery ('..-battery-type') is left alone on purpose — a streaming
// host runs on AC, and pinning a laptop's battery behaviour would be rude.
const char* kSessionSchema = "org.gnome.desktop.session";
const char* kPowerSchema = "org.gnome.settings-daemon.plugins.power";

QString gsettings()
{
    static const QString path = QStandardPaths::findExecutable(QStringLiteral("gsettings"));
    return path;
}

// Run gsettings synchronously. Returns false on a non-zero exit, a failure to
// start, or a timeout; `out` receives stdout+stderr when provided. Short timeout:
// these are local dconf reads/writes, so anything slow is already broken.
bool runGsettings(const QStringList& args, QString* out = nullptr)
{
    const QString bin = gsettings();
    if (bin.isEmpty()) return false;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(bin, args);
    if (!proc.waitForStarted(5000)) return false;
    if (!proc.waitForFinished(10000)) {
        proc.kill();
        proc.waitForFinished(2000);
        return false;
    }
    if (out) *out = QString::fromUtf8(proc.readAll()).trimmed();
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// A schema missing from this build of GNOME (or a non-GNOME desktop entirely)
// makes every `gsettings set` on it fail loudly, so probe before touching it.
bool hasSchema(const char* schema)
{
    QString out;
    // `list-keys` fails on an unknown schema; `list-schemas | grep` would need a
    // shell and pull in the whole schema list for one answer.
    return runGsettings({QStringLiteral("list-keys"), QString::fromLatin1(schema)}, &out);
}

} // namespace

bool isSupported()
{
    // No desktop session (server, container, MW_SERVICE unit) → there is no
    // display to keep awake and no user dconf to write to.
    if (!mw::hasDesktopSession()) return false;
    if (gsettings().isEmpty()) return false;
    return hasSchema(kSessionSchema) && hasSchema(kPowerSchema);
}

bool isDisplayKeptAwake()
{
    if (!isSupported()) return false;

    QString idle;
    if (!runGsettings({QStringLiteral("get"), QString::fromLatin1(kSessionSchema),
                       QStringLiteral("idle-delay")},
                      &idle))
        return false;
    // gsettings prints the GVariant, not the bare number: "uint32 0".
    if (!idle.endsWith(QStringLiteral(" 0")) && idle != QStringLiteral("0")) return false;

    QString sleepType;
    if (!runGsettings({QStringLiteral("get"), QString::fromLatin1(kPowerSchema),
                       QStringLiteral("sleep-inactive-ac-type")},
                      &sleepType))
        return false;
    return sleepType == QStringLiteral("'nothing'");
}

QString keepDisplayAwake()
{
    if (!isSupported())
        return QStringLiteral("This desktop does not expose the display-blanking setting.");

    QString err;
    if (!runGsettings({QStringLiteral("set"), QString::fromLatin1(kSessionSchema),
                       QStringLiteral("idle-delay"), QStringLiteral("0")},
                      &err)) {
        qWarning() << "[DisplaySleep] could not disable screen blanking:" << err;
        return err.isEmpty() ? QStringLiteral("Could not disable screen blanking.") : err;
    }
    if (!runGsettings({QStringLiteral("set"), QString::fromLatin1(kPowerSchema),
                       QStringLiteral("sleep-inactive-ac-type"), QStringLiteral("nothing")},
                      &err)) {
        // Blanking is already off, which is the part that breaks capture — report
        // the failure but don't pretend nothing was applied.
        qWarning() << "[DisplaySleep] could not disable idle suspend:" << err;
        return err.isEmpty() ? QStringLiteral("Could not disable idle suspend.") : err;
    }
    qInfo() << "[DisplaySleep] display set never to blank or sleep on AC";
    return QString();
}

#else // Windows / macOS — see the header for why.

bool isSupported()
{
    return false;
}

bool isDisplayKeptAwake()
{
    return false;
}

QString keepDisplayAwake()
{
    return QStringLiteral("Not supported on this platform.");
}

#endif

} // namespace DisplaySleep
