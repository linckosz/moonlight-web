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

#include "SelfUpdater.h"
#include "AddressRace.h"
#include "RelaunchScript.h"
#include "UpdateChecker.h"
#include "common/Edition.h"
#include "common/DetachedProcess.h"
#include "common/Logger.h"
#include "common/WinSystemPath.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#ifdef Q_OS_WIN
// Registered by the installer (which runs elevated) with RunLevel=HighestAvailable
// and no trigger; `schtasks /Run` on it elevates without a UAC prompt.
// Named after the edition, like everything the installer registers: a DEV
// install beside a production one must never start the other's task.
QString updateTaskName()
{
    return mw::edition::productName() + QStringLiteral(" Update");
}

bool updateTaskExists()
{
    QProcess p;
    p.start(QStringLiteral("schtasks.exe"),
            {QStringLiteral("/Query"), QStringLiteral("/TN"), updateTaskName()});
    if (!p.waitForFinished(10000)) {
        p.kill();
        return false;
    }
    return p.exitCode() == 0;
}

// Transient task used by the "service" path to get the installer OUT of our
// process tree. Recreated (/F) on every attempt, so a leftover is harmless — it
// points at a staged file the next startup deletes.
QString serviceTaskName()
{
    return mw::edition::productName() + QStringLiteral(" Update (service)");
}

QString xmlEscape(const QString& s)
{
    QString out = s;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    out.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return out;
}
#else
// The install commands are assembled as one shell string so the whole sequence
// (install → stop us → relaunch) survives this process being killed halfway
// through it.
using relaunchscript::shQuote;

bool runningAsRoot()
{
    return ::geteuid() == 0;
}

#ifdef Q_OS_MACOS
// The .app this process runs from (…/X.app/Contents/MacOS/X → …/X.app), or
// empty for a bare binary outside any bundle (a build tree, a bench tool).
QString appBundlePath()
{
    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.cdUp() || !dir.cdUp()) return {};
    const QString path = dir.absolutePath();
    return path.endsWith(QLatin1String(".app")) ? path : QString();
}

// True when this user can rewrite the whole bundle without asking anyone: the
// .pkg's postinstall hands it to the console user, the way Chrome and Firefox
// own theirs. A bundle still root-owned — every install made before that, or
// another account's — falls back to the password prompt.
bool bundleOwnedByUs()
{
    const QString bundle = appBundlePath();
    if (bundle.isEmpty()) return false;
    const uid_t me = ::geteuid();
    // The spots a stray root-owned leftover would surface first; postinstall
    // chowns the tree recursively, so these stand for the rest.
    for (const QString& p :
         {bundle, bundle + QStringLiteral("/Contents"), bundle + QStringLiteral("/Contents/MacOS"),
          QCoreApplication::applicationFilePath()}) {
        struct stat st{};
        if (::stat(QFile::encodeName(p).constData(), &st) != 0 || st.st_uid != me) return false;
    }
    return true;
}
#else
// A container's filesystem is not where this application lives — the image is.
// Installing a .deb into it would appear to work and then vanish at the next
// `docker run`, silently reverting the update and leaving the operator to
// wonder why the version went backwards. The image registry is the update
// mechanism here, so there is no in-place path to offer at all.
//
// Linux only, matching its single caller below: on macOS this would be an
// unused static function and a warning in the packaging build.
bool runningInContainer()
{
    if (QFile::exists(QStringLiteral("/.dockerenv")) ||
        QFile::exists(QStringLiteral("/run/.containerenv")))
        return true;
    QFile cgroup(QStringLiteral("/proc/1/cgroup"));
    if (!cgroup.open(QIODevice::ReadOnly)) return false;
    const QByteArray content = cgroup.readAll();
    return content.contains("docker") || content.contains("lxc") || content.contains("containerd");
}

// The root helper the .deb/.rpm ships next to the binary (production only):
// it upgrades the package from its own signed repository and takes no
// argument, so the polkit rule that lets the active session run it without a
// password lets it do that and nothing else.
QString repoHelperPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/moonlightweb-update");
}

// Everything the "repo" path needs, all put there by the package's postinst:
// the helper, the polkit action that waives the password for it, pkexec to
// call it, and the repository it upgrades from.
bool repoUpdateReady()
{
    const QFileInfo helper(repoHelperPath());
    if (!helper.isFile() || !helper.isExecutable()) return false;
    if (!QFile::exists(
            QStringLiteral("/usr/share/polkit-1/actions/top.moonlightweb.update.policy")))
        return false;
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) return false;
    return QFile::exists(QStringLiteral("/etc/apt/sources.list.d/moonlightweb.sources")) ||
           QFile::exists(QStringLiteral("/etc/yum.repos.d/moonlightweb.repo")) ||
           QFile::exists(QStringLiteral("/etc/zypp/repos.d/moonlightweb.repo"));
}
#endif // Q_OS_MACOS
#endif

} // namespace

SelfUpdater::SelfUpdater(UpdateChecker* checker, QObject* parent)
    : QObject(parent)
    , m_checker(checker)
    , m_nam(new QNetworkAccessManager(this))
{
    // Drop the installer a previous update staged (~60 MB). It cannot be removed
    // at the end of that update — the installer is still running it when it
    // relaunches us — so the next startup is the first chance we get.
    const QDir dir(stagingDir());
    if (dir.exists()) {
        for (const QFileInfo& fi : dir.entryInfoList(QDir::Files))
            QFile::remove(fi.absoluteFilePath());
        // macOS "bundle" path: the unpacked .pkg (~150 MB) the swap could not
        // remove if it was killed halfway. A no-op everywhere else.
        QDir(dir.filePath(QStringLiteral("expanded-") + mw::edition::productName()))
            .removeRecursively();
    }
#ifdef Q_OS_WIN
    // Same reasoning for the transient task the service path registers: it can
    // only be dropped once the update it was running is over. Detached — a
    // startup must not wait on schtasks.
    QProcess::startDetached(QStringLiteral("schtasks.exe"),
                            {QStringLiteral("/Delete"), QStringLiteral("/TN"), serviceTaskName(),
                             QStringLiteral("/F")});
#endif
}

QString SelfUpdater::stagingDir()
{
#ifdef Q_OS_WIN
    // Per-user, NOT %ProgramData%: this directory holds an executable we run
    // elevated, so it must not be writable by other local accounts.
    // Under the edition's own name: the installer's elevated task runs a fixed
    // path, and a DEV install must never stage into production's.
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty() && !mw::win::isUnderSystem32(base))
        return base + QLatin1Char('/') + mw::edition::productName() + QStringLiteral("/update");
    // A service has either no LOCALAPPDATA or SYSTEM's own, under System32 —
    // where the 32-bit installer cannot find itself (see isUnderSystem32), so
    // every update from a service used to die there silently. Falling back to the
    // temp directory would put the installer in C:\Windows\Temp — a directory
    // every local account can write to, holding a file we are about to execute
    // as SYSTEM. The install directory is admin-only, which is the property that
    // actually matters.
    return QCoreApplication::applicationDirPath() + QStringLiteral("/update");
#else
    return QDir::tempPath() + QStringLiteral("/MoonlightWeb-update");
#endif
}

QString SelfUpdater::stagedFileName(const QString& assetName)
{
#ifdef Q_OS_WIN
    Q_UNUSED(assetName);
    // Fixed name — the scheduled task's action points at this exact path.
    return QStringLiteral("MoonlightWeb-update.exe");
#else
    // Keep the extension: it selects the install command (.deb/.rpm/.AppImage/.pkg).
    const QString name = QFileInfo(assetName).fileName();
    return name.isEmpty() ? QStringLiteral("MoonlightWeb-update") : name;
#endif
}

QString SelfUpdater::elevationMethod()
{
    // Computed once: /api/update/check is polled by every hosts page, and the
    // Windows branch shells out to schtasks. Nothing here changes without an
    // install, which restarts the process anyway.
    static const QString cached = [] {
#if defined(Q_OS_WIN)
        // A service-managed instance already runs as SYSTEM: nothing to elevate.
        if (!qEnvironmentVariableIsEmpty("MW_SERVICE")) return QStringLiteral("service");
        if (updateTaskExists()) return QStringLiteral("scheduled-task");
        return QStringLiteral("direct"); // UAC consent on the host desktop
#elif defined(Q_OS_MACOS)
        if (runningAsRoot()) return QStringLiteral("root");
        if (bundleOwnedByUs()) return QStringLiteral("bundle"); // replace in place, no root
        return QStringLiteral("osascript"); // password prompt on the host desktop
#else
        // Checked before root, precisely because a container usually IS root:
        // without this the update would look supported, install into the image's
        // writable layer, and be thrown away by the next `docker run`.
        if (runningInContainer()) return QString();
        // An AppImage lives in the user's own filesystem — replacing it needs no
        // privilege at all, which makes it the only always-silent path on Linux.
        if (!qEnvironmentVariableIsEmpty("APPIMAGE")) return QStringLiteral("appimage");
        if (runningAsRoot()) return QStringLiteral("root");
        // A package from our repository, upgraded by the helper polkit lets
        // the active session run without a password.
        if (repoUpdateReady()) return QStringLiteral("repo");
        if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty())
            return QStringLiteral("pkexec"); // polkit prompt on the host desktop
        return QString();
#endif
    }();
    return cached;
}

bool SelfUpdater::methodNeedsHostConfirmation(const QString& method)
{
    return method == QLatin1String("direct") || method == QLatin1String("pkexec") ||
           method == QLatin1String("osascript");
}

QJsonObject SelfUpdater::capabilityJson() const
{
    const QString method = elevationMethod();
    QJsonObject obj;
    obj["supported"] = !method.isEmpty();
    obj["method"] = method;
    obj["requires_host_confirmation"] = methodNeedsHostConfirmation(method);
    return obj;
}

QJsonObject SelfUpdater::statusJson() const
{
    QString state;
    switch (m_state) {
    case State::Downloading: state = QStringLiteral("downloading"); break;
    case State::Installing: state = QStringLiteral("installing"); break;
    case State::Restarting: state = QStringLiteral("restarting"); break;
    case State::Failed: state = QStringLiteral("failed"); break;
    case State::Idle: state = QStringLiteral("idle"); break;
    }

    QJsonObject obj;
    obj["state"] = state;
    obj["percent"] = m_percent;
    obj["version"] = m_target;
    obj["requires_host_confirmation"] =
        m_state != State::Idle && methodNeedsHostConfirmation(m_method);
    if (!m_error.isEmpty()) obj["error"] = m_error;
    return obj;
}

QString SelfUpdater::start()
{
    if (m_state == State::Downloading || m_state == State::Installing ||
        m_state == State::Restarting)
        return QStringLiteral("An update is already in progress");

    const QString method = elevationMethod();
    if (method.isEmpty())
        return QStringLiteral("This platform has no unattended update path — "
                              "update MoonlightWeb on the host manually");

    m_method = method;
    m_recheckDone = false;
    return beginDownload();
}

QString SelfUpdater::beginDownload()
{
    const QJsonObject info = m_checker->statusJson();
    if (!info.value("update_available").toBool()) return QStringLiteral("No update available");
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Nothing to download: the helper takes the package from the repository,
    // where apt or dnf checks its signature. The repository is built from the
    // very release the checker reports, so it holds that version.
    if (m_method == QLatin1String("repo")) {
        m_target = info.value("latest").toString();
        m_pkgPath.clear();
        m_error.clear();
        startInstallPhase();
        return {};
    }
#endif
    // An empty asset_name means UpdateChecker fell back to the release page:
    // there is no installer to run for this OS/arch.
    const QString assetName = info.value("asset_name").toString();
    const QString url = info.value("download_url").toString();
    if (assetName.isEmpty() || url.isEmpty())
        return QStringLiteral("No installer published for this platform");

    const QString dir = stagingDir();
    if (!QDir().mkpath(dir)) return QStringLiteral("Cannot create the update staging directory");

    m_pkgPath = dir + QLatin1Char('/') + stagedFileName(assetName);
    // A leftover from an interrupted attempt would be silently reused by the
    // scheduled task, so always start from a clean file.
    QFile::remove(m_pkgPath);

    m_file = new QFile(m_pkgPath, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        delete m_file;
        m_file = nullptr;
        return QStringLiteral("Cannot write to the update staging directory");
    }

    m_target = info.value("latest").toString();
    m_expectedSize = static_cast<qint64>(info.value("asset_size").toDouble());
    m_expectedDigest = info.value("asset_digest").toString();
    m_error.clear();
    m_state = State::Downloading;
    m_percent = 0;

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", "MoonlightWeb-SelfUpdater");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    AddressRace::enableOn(req);
    m_reply = m_nam->get(req);

    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_file) m_file->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) m_percent = static_cast<int>(got * kDownloadShare / total);
    });
    connect(m_reply, &QNetworkReply::finished, this, &SelfUpdater::onDownloadFinished);

    Logger::info(
        QStringLiteral("[Update] downloading %1 (%2) via %3").arg(m_target, url, m_method));
    return QString();
}

void SelfUpdater::onDownloadFinished()
{
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (m_file) {
        m_file->write(reply->readAll());
        m_file->close();
    }

    if (reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("Download failed: %1").arg(reply->errorString()));
        return;
    }
    // Verify what we got against what GitHub said the asset is, BEFORE handing
    // it to an installer that reports nothing. An installer whose bytes are not
    // the published ones fails its own integrity check, writes no log, and dies
    // with a bare exit code — which is a miserable thing to debug from the other
    // side of the planet. A release whose asset is replaced between the check
    // and the download is enough to land here.
    const qint64 size = m_file ? m_file->size() : 0;
    delete m_file;
    m_file = nullptr;

    // Both guards below compare against metadata UpdateChecker cached, possibly
    // hours ago (kCacheHours). Re-publishing a release — deleting it and pushing
    // the tag again, which rebuilds every asset — keeps the asset name and the
    // download URL but changes the bytes behind them, so a stale cache
    // mismatches on a download that is in fact perfectly good. That is the
    // likely case here, and it fixes itself: refetch and replay, once.
    if (m_expectedSize > 0 && size != m_expectedSize) {
        const QString why = QStringLiteral("Downloaded installer is %1 bytes, expected %2")
                                .arg(size)
                                .arg(m_expectedSize);
        if (retryOnStaleMetadata(why)) return;
        fail(why + QStringLiteral(" — the published asset does not match what this download "
                                  "produced; install it manually from the releases page"));
        return;
    }
    if (m_expectedSize <= 0 && size < 1024 * 1024) {
        // No published size to compare against: fall back to catching the
        // obvious case (an HTML error page saved as an installer).
        fail(QStringLiteral("Downloaded installer looks truncated (%1 bytes)").arg(size));
        return;
    }
    if (const QString bad = verifyDigest(); !bad.isEmpty()) {
        if (retryOnStaleMetadata(bad)) return;
        fail(bad);
        return;
    }

    startInstallPhase();
}

void SelfUpdater::startInstallPhase()
{
    m_state = State::Installing;
    m_percent = kDownloadShare;
    m_installElapsedMs = 0;

    // Pseudo-progress: the installers give us nothing to read, and on Windows
    // this process is killed partway through, so the bar just has to keep
    // moving until the client loses us. Exponential rather than linear — a
    // linear crawl either hits its cap long before the install ends (and then
    // freezes, which is exactly what a stuck update looks like) or is too slow
    // to read as motion. This never reaches kInstallCeiling.
    m_installTick = new QTimer(this);
    connect(m_installTick, &QTimer::timeout, this, [this]() {
        m_installElapsedMs += kInstallTickMs;
        const double eased = 1.0 - std::exp(-(m_installElapsedMs / 1000.0) / kInstallTauSec);
        m_percent = kDownloadShare + static_cast<int>((kInstallCeiling - kDownloadShare) * eased);
    });
    m_installTick->start(kInstallTickMs);

    runInstaller();
}

bool SelfUpdater::retryOnStaleMetadata(const QString& reason)
{
    // Exactly one extra attempt per start(): if the freshly fetched metadata
    // disagrees with the bytes too, the file really is wrong and downloading it
    // a third time will not change that.
    if (m_recheckDone) return false;
    m_recheckDone = true;

    Logger::warning(
        QStringLiteral("[Update] %1 — re-checking the release and retrying").arg(reason));
    m_percent = 0; // stays State::Downloading: from the UI's side this is one attempt

    // Wait for the check, never *on* it. A host that just lost its link would
    // otherwise sit on "downloading" until the watchdog that does not exist yet
    // fires, which is to say forever.
    m_recheckGuard = new QTimer(this);
    m_recheckGuard->setSingleShot(true);
    connect(m_recheckGuard, &QTimer::timeout, this, [this, reason]() {
        disconnect(m_recheckWait);
        m_recheckGuard->deleteLater();
        m_recheckGuard = nullptr;
        fail(reason + QStringLiteral(" — and the release could not be re-checked; try again"));
    });

    m_recheckWait = connect(
        m_checker, &UpdateChecker::checkFinished, this,
        [this, reason]() {
            m_recheckGuard->stop();
            m_recheckGuard->deleteLater();
            m_recheckGuard = nullptr;
            // beginDownload() re-reads the checker, so this now carries whatever
            // the release actually holds — including "no update available" if it
            // was pulled entirely.
            if (const QString err = beginDownload(); !err.isEmpty())
                fail(reason + QStringLiteral(" — ") + err);
        },
        Qt::SingleShotConnection);

    m_recheckGuard->start(kRecheckTimeoutMs);
    m_checker->refresh();
    return true;
}

QString SelfUpdater::verifyDigest()
{
    // GitHub publishes it as "sha256:<hex>"; anything else (or nothing at all,
    // on an older API response) leaves the size check as the only guard.
    if (!m_expectedDigest.startsWith(QLatin1String("sha256:"), Qt::CaseInsensitive)) return {};
    const QString want = m_expectedDigest.mid(7).trimmed().toLower();
    if (want.isEmpty()) return {};

    QFile f(m_pkgPath);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("Cannot re-read the downloaded installer to verify it");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return QStringLiteral("Could not hash the downloaded installer");
    const QString got = QString::fromLatin1(hash.result().toHex());
    if (got != want)
        return QStringLiteral("Downloaded installer does not match the published sha256 "
                              "(%1 vs %2) — try again")
            .arg(got.left(12), want.left(12));
    Logger::info(QStringLiteral("[Update] installer verified: %1 bytes, sha256 %2")
                     .arg(m_expectedSize)
                     .arg(want.left(12)));
    return {};
}

QString SelfUpdater::installerLogPath()
{
    // Next to our own log, NOT in the staging directory: that one is wiped on the
    // next startup, which is exactly when someone comes looking for the log of
    // the update that just failed.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#ifdef Q_OS_WIN
    // A service's AppDataLocation is under System32, which the 32-bit installer
    // sees as SysWOW64: it could not create the log there.
    if (mw::win::isUnderSystem32(base)) base = QCoreApplication::applicationDirPath();
#endif
    QDir().mkpath(base + QStringLiteral("/logs"));
    return base + QStringLiteral("/logs/installer.log");
}

#ifdef Q_OS_WIN
QString SelfUpdater::startViaSystemTask(const QString& program, const QStringList& args)
{
    // Registered from XML rather than /TR: the action carries a quoted path and
    // switches, and schtasks' own parsing of that string is a minefield. This is
    // the same approach the installer uses for its own tasks.
    const QString xmlPath = stagingDir() + QStringLiteral("/update-task.xml");
    QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        "  <RegistrationInfo><Author>MoonlightWeb</Author></RegistrationInfo>\r\n"
        "  <Principals><Principal id=\"Author\">"
        "<UserId>S-1-5-18</UserId>" // LocalSystem: we are already SYSTEM, so no prompt
        "<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        "  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        "<AllowHardTerminate>false</AllowHardTerminate>"
        "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
        "<Enabled>true</Enabled></Settings>\r\n"
        "  <Actions Context=\"Author\"><Exec><Command>%1</Command>"
        "<Arguments>%2</Arguments></Exec></Actions>\r\n"
        "</Task>\r\n");
    // <Arguments> is one flat string, so anything holding a space (the /LOG path
    // under a user profile like "John Doe") has to carry its own quotes.
    QStringList quoted;
    for (const QString& a : args)
        quoted << (a.contains(u' ') ? QLatin1Char('"') + a + QLatin1Char('"') : a);
    xml = xml.arg(xmlEscape(QDir::toNativeSeparators(program)), xmlEscape(quoted.join(u' ')));

    QFile f(xmlPath);
    if (!f.open(QIODevice::WriteOnly)) return QStringLiteral("Cannot write the update task");
    // UTF-16LE with a BOM — what schtasks expects for an XML definition.
    const char16_t bom = 0xFEFF;
    f.write(reinterpret_cast<const char*>(&bom), 2);
    const std::u16string body = xml.toStdU16String();
    f.write(reinterpret_cast<const char*>(body.data()), qint64(body.size() * 2));
    f.close();

    const QString name = serviceTaskName();
    QProcess reg;
    reg.start(QStringLiteral("schtasks.exe"),
              {QStringLiteral("/Create"), QStringLiteral("/TN"), name, QStringLiteral("/XML"),
               xmlPath, QStringLiteral("/F")});
    if (!reg.waitForFinished(20000) || reg.exitCode() != 0)
        return QStringLiteral("Could not register the update task (%1)")
            .arg(QString::fromLocal8Bit(reg.readAllStandardError()).trimmed());

    // /Run returns as soon as the task is queued. Its own child is the installer,
    // so nothing here is in our process tree any more.
    QProcess run;
    run.start(QStringLiteral("schtasks.exe"),
              {QStringLiteral("/Run"), QStringLiteral("/TN"), name});
    if (!run.waitForFinished(20000) || run.exitCode() != 0)
        return QStringLiteral("Could not start the update task (%1)")
            .arg(QString::fromLocal8Bit(run.readAllStandardError()).trimmed());
    return {};
}
#endif

void SelfUpdater::runInstaller()
{
    Logger::info(QStringLiteral("[Update] applying %1 via %2").arg(m_target, m_method));

#if defined(Q_OS_WIN)
    // Inno silent switches: no wizard, no message boxes, no reboot. Update mode
    // is auto-detected by the installer from the existing install, so every page
    // is skipped and the current configuration is preserved.
    // The /LOG is not a debugging leftover: with message boxes suppressed it is
    // the ONLY account of what the installer did, and it is written by a process
    // that outlives us — so it is the only evidence that can survive a failed
    // update at all. Costs nothing, answers everything.
    const QStringList silent{QStringLiteral("/VERYSILENT"), QStringLiteral("/SUPPRESSMSGBOXES"),
                             QStringLiteral("/NORESTART"), QStringLiteral("/SP-"),
                             QStringLiteral("/LOG=") + installerLogPath()};

    // A service must NOT be the installer's parent. Inno's Restart Manager pass
    // shuts down the application holding the files it is replacing — i.e. us —
    // and NSSM takes the whole process tree down with the service, installer
    // included, the very moment it starts working. Verified on a service install:
    // launched as our child it silently does nothing; the identical command run
    // by the task scheduler as SYSTEM installs cleanly.
    if (m_method == QLatin1String("service")) {
        const QString err = startViaSystemTask(m_pkgPath, silent);
        if (!err.isEmpty()) {
            fail(err);
            return;
        }
        m_state = State::Restarting;
        armWatchdog();
        return;
    }

    QString program = m_pkgPath; // "direct" — UAC consent on the host desktop
    QStringList args = silent;
    if (m_method == QLatin1String("scheduled-task")) {
        // The task's action already carries the installer path + silent switches.
        program = QStringLiteral("schtasks.exe");
        args = {QStringLiteral("/Run"), QStringLiteral("/TN"), updateTaskName()};
    }

    // A tracked child rather than startDetached: an installer that declines to
    // run leaves NO trace anywhere (/SUPPRESSMSGBOXES turns every diagnostic
    // into an exit code and nothing else), and that code is the only thing that
    // tells the difference between "still working" and "never started". Nothing
    // depends on the child outliving us — Inno's SetupLdr waits for the real
    // setup and forwards its exit code, and the setup that replaces this process
    // is a grandchild that survives us being killed.
    m_installer = new QProcess(this);
    connect(m_installer, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        onInstallerFinished(code, status != QProcess::NormalExit);
    });
    m_installer->start(program, args);
    if (!m_installer->waitForStarted(10000)) {
        fail(QStringLiteral("Could not launch the installer: %1").arg(m_installer->errorString()));
        return;
    }
#else
#if defined(Q_OS_MACOS)
    if (m_method == QLatin1String("bundle")) {
        prepareBundle();
        return;
    }
#endif
    const QString pkg = shQuote(m_pkgPath);
    QString install;

#if defined(Q_OS_MACOS)
    const QString cmd = QStringLiteral("/usr/sbin/installer -pkg %1 -target /").arg(pkg);
    if (m_method == QLatin1String("root"))
        install = cmd;
    else
        install = QStringLiteral("/usr/bin/osascript -e %1")
                      .arg(shQuote(QStringLiteral("do shell script \"%1\" with administrator "
                                                  "privileges")
                                       .arg(cmd)));
#else
    if (m_method == QLatin1String("repo")) {
        // pkexec, but against our own polkit action: no prompt. Logged, since
        // apt's output is the only account of an upgrade that went wrong.
        install = QStringLiteral("pkexec %1 >>%2 2>&1")
                      .arg(shQuote(repoHelperPath()), shQuote(installerLogPath()));
    } else if (m_method == QLatin1String("appimage")) {
        // Replace the AppImage in place — no privilege needed, and rename() over
        // a running AppImage is safe (the mounted image keeps the old inode).
        const QString target = shQuote(qEnvironmentVariable("APPIMAGE"));
        install = QStringLiteral("chmod 755 %1 && mv -f %1 %2").arg(pkg, target);
    } else {
        QString tool;
        if (m_pkgPath.endsWith(QLatin1String(".deb"), Qt::CaseInsensitive))
            tool = QStringLiteral("apt-get install -y --allow-downgrades %1").arg(pkg);
        else if (m_pkgPath.endsWith(QLatin1String(".rpm"), Qt::CaseInsensitive))
            tool = QStringLiteral("rpm -U --force %1").arg(pkg);
        else {
            fail(QStringLiteral("Unsupported package type for an unattended update"));
            return;
        }
        install = m_method == QLatin1String("root")
                      ? tool
                      : QStringLiteral("pkexec /bin/sh -c %1").arg(shQuote(tool));
    }
#endif
    if (!launchDetached(install)) return;
#endif

    m_state = State::Restarting;
    armWatchdog();
}

#ifndef Q_OS_WIN
bool SelfUpdater::launchDetached(const QString& install)
{
    // One detached shell sequence: install, stop this process, relaunch. It has
    // to outlive us, which is exactly what startDetached's session detachment
    // buys — a plain child would die with the parent it is about to kill.
    // Relaunching the binary directly (rather than `open -a` on macOS) keeps one
    // code path and goes through the app's own single-instance logic. With the
    // settings file it was started on: without --config the updated server
    // would come back on the default one.
    QString exe = QCoreApplication::applicationFilePath();
#if !defined(Q_OS_MACOS)
    // A package starts us through moonlightweb-launch, which carries the
    // capabilities the screen capture needs; the binary alone would come back
    // unable to capture. (An AppImage has no launcher and relaunches itself.)
    const QString launcher =
        QCoreApplication::applicationDirPath() + QStringLiteral("/moonlightweb-launch");
    if (qEnvironmentVariableIsEmpty("APPIMAGE") && QFileInfo(launcher).isExecutable())
        exe = launcher;
#endif
    // The .deb/.rpm maintainer scripts stop the app, and start it again only
    // when it is already gone by then; the .pkg's postinstall kickstarts its
    // LaunchAgent. This relaunch covers whatever they left down (AppImage, a
    // .deb whose old app was still exiting) and is harmless elsewhere — the
    // single-instance lock turns the extra launch into a silent no-op. Under a
    // LaunchAgent, launchd does the relaunch instead (see RelaunchScript.h).
    QString service;
    QString plist;
#if defined(Q_OS_MACOS)
    // launchd names its job in our environment; only our own agents count, not
    // a bench plist or a terminal started from some other job.
    const QString job = qEnvironmentVariable("XPC_SERVICE_NAME");
    if (job.startsWith(QLatin1String("com.moonlightweb.agent"))) {
        service = QStringLiteral("gui/%1/%2").arg(::getuid()).arg(job);
        // The bundle swap runs no installer, so the job is loaded anew from
        // here. A .pkg's postinstall already does it, and doing it again
        // afterwards would only restart the new server a second time.
        // The path is where the postinstall and the start-at-login switch
        // write it.
        const QString path = QDir::homePath() + QStringLiteral("/Library/LaunchAgents/") + job +
                             QStringLiteral(".plist");
        if (m_method == QLatin1String("bundle") && QFileInfo::exists(path)) plist = path;
    }
#endif
    const QString script = relaunchscript::build(install, QCoreApplication::applicationPid(), exe,
                                                 AppSettings::fileOverride(), service, plist);

    if (!mw::startDetached(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script})) {
        fail(QStringLiteral("Could not launch the installer"));
        return false;
    }
    return true;
}
#endif

#ifdef Q_OS_MACOS
void SelfUpdater::prepareBundle()
{
    // The user-owned bundle path: no installer at all. The .pkg is unpacked
    // without privilege, its app checked, then copied over ours — the way an
    // AppImage replaces itself. What only a root postinstall does (LaunchAgent,
    // firewall entry) was done by the first install and carries over as is.
    const QString bundle = appBundlePath();
    const QString work = stagingDir() + QStringLiteral("/expanded-") + mw::edition::productName();
    const QString fresh = work + QStringLiteral("/new.app");

    // The new app must answer the same designated requirement as ours. That is
    // what TCC keys Screen Recording and Accessibility on, so an app that fails
    // it would come back unable to capture; it is also the certificate pin that
    // makes a substituted payload worthless. An ad hoc build (CI without the
    // signing secrets) has a cdhash requirement no other build can meet, so it
    // only gets the integrity check — the GitHub digest already vouched for it.
    QString requirement;
    {
        QProcess cs;
        cs.setProcessChannelMode(QProcess::MergedChannels);
        cs.start(QStringLiteral("/usr/bin/codesign"),
                 {QStringLiteral("-d"), QStringLiteral("-r-"), bundle});
        if (cs.waitForFinished(10000)) {
            const QString out = QString::fromUtf8(cs.readAll());
            for (const QString& line : out.split(QLatin1Char('\n'))) {
                if (line.startsWith(QLatin1String("designated => ")) &&
                    line.contains(QLatin1String("certificate")))
                    requirement = line.mid(14).trimmed();
            }
        } else {
            cs.kill();
        }
    }
    const QString test = requirement.isEmpty() ? QString()
                                               : QStringLiteral(" --test-requirement ") +
                                                     shQuote(QLatin1Char('=') + requirement);

    // Product archive → component package → Payload → X.app; found by name
    // rather than by that layout, and moved to a fixed path for the next steps.
    const QString name = QFileInfo(bundle).fileName();
    const QString script =
        QStringLiteral("rm -rf %1 && /usr/sbin/pkgutil --expand-full %2 %1 && "
                       "src=$(/usr/bin/find %1 -maxdepth 3 -type d -name %3 | /usr/bin/head -n 1) "
                       "&& [ -n \"$src\" ] && mv \"$src\" %4 && "
                       "/usr/bin/codesign --verify --deep --strict%5 %4")
            .arg(shQuote(work), shQuote(m_pkgPath), shQuote(name), shQuote(fresh), test);

    Logger::info(QStringLiteral("[Update] unpacking into the user-owned bundle %1 (%2)")
                     .arg(bundle, requirement.isEmpty() ? QStringLiteral("integrity check only")
                                                        : requirement));
    // Covers the unpacking too: a pkgutil that never returns must not leave the
    // bar stuck on "installing".
    armWatchdog();

    m_installer = new QProcess(this);
    m_installer->setProcessChannelMode(QProcess::MergedChannels);
    m_installer->setStandardOutputFile(installerLogPath());
    connect(m_installer, &QProcess::finished, this,
            [this, bundle, work, fresh](int code, QProcess::ExitStatus status) {
                m_installer->deleteLater();
                m_installer = nullptr;
                // The watchdog gave up on a slow unpack: too late to swap now.
                if (m_state != State::Installing) {
                    QDir(work).removeRecursively();
                    return;
                }
                if (status != QProcess::NormalExit || code != 0) {
                    QDir(work).removeRecursively();
                    fail(QStringLiteral("The update could not be unpacked, or its signature "
                                        "does not match this app (see logs/installer.log)"));
                    return;
                }
                // --delete: a file the new version dropped would otherwise stay
                // in the bundle, unsealed, and break its signature. rsync writes
                // each file aside and renames it in, so the running binary keeps
                // its old inode until the kill.
                const QString swap =
                    QStringLiteral("/usr/bin/rsync -a --delete %1/ %2/ >>%3 2>&1; rm -rf %4")
                        .arg(shQuote(fresh), shQuote(bundle), shQuote(installerLogPath()),
                             shQuote(work));
                if (launchDetached(swap)) m_state = State::Restarting;
            });
    m_installer->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
}
#endif

void SelfUpdater::armWatchdog()
{
    // Every path is supposed to end with this process being replaced. If we are
    // still alive when this fires, it did not happen — and without it the state
    // machine would sit in "restarting" forever: the bar can never complete, and
    // every retry of /api/update/start is rejected with "already in progress"
    // until someone restarts the server by hand.
    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, [this]() {
        // Keep the staged installer: a prompt may still be waiting for someone
        // on the host desktop, and it is that file it would run.
        fail(QStringLiteral("The installer did not go through — MoonlightWeb is still "
                            "running the old version. Check the host PC."),
             false);
    });
    // A prompt on the host desktop needs a human to walk over to it; the silent
    // paths have no such excuse.
    m_watchdog->start(methodNeedsHostConfirmation(m_method) ? 15 * 60 * 1000 : 3 * 60 * 1000);
}

void SelfUpdater::onInstallerFinished(int exitCode, bool crashed)
{
    // Reaching this at all is the unexpected case: the installer normally takes
    // this process down long before it is done.
    if (crashed) {
        fail(QStringLiteral("The installer crashed before applying the update"));
        return;
    }
    if (exitCode != 0) {
        fail(QStringLiteral("The installer exited with code %1 without applying the update")
                 .arg(exitCode));
        return;
    }
    // Exit code 0 while we are still running: either the launcher just queued
    // the work (scheduled task) or the install is being finished by a process
    // that outlived its parent. Let the watchdog be the judge.
    Logger::info(QStringLiteral("[Update] installer launcher exited cleanly, still waiting"));
}

void SelfUpdater::fail(const QString& message, bool dropStaged)
{
    if (m_installTick) {
        m_installTick->stop();
        m_installTick->deleteLater();
        m_installTick = nullptr;
    }
    if (m_watchdog) {
        m_watchdog->stop();
        m_watchdog->deleteLater();
        m_watchdog = nullptr;
    }
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    if (dropStaged) QFile::remove(m_pkgPath);
    m_state = State::Failed;
    m_error = message;
    Logger::warning(QStringLiteral("[Update] %1").arg(message));
}
