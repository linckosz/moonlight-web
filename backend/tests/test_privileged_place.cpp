/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Where a process that runs with more rights than its user keeps its own files
 * (common/PrivilegedPlace): the directories it makes and what it accepts of
 * them, the log it opens there, and what it deletes.
 *
 * What these tests hold. The directories are made with the access they are
 * documented to have, and a directory that is not one of them — a junction,
 * one another account may write to, one owned by someone else — is not used:
 * it is made again, or no file is written. A log is opened only as a regular
 * file with a single name, in one of those directories; the logger says so and
 * comes back when the file can be had. A delete never goes through a junction.
 * The archive of logs reads this process's directory and, for a process with
 * its user's rights, that account's own directory under host-data.
 *
 * The TNR runs without the rights the real directories need, so the tests name
 * this process's own account as the one that owns and may write them
 * (mw::place::setForTests).
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <sddl.h>
// clang-format on
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "test_framework.h"

#include "common/Logger.h"
#include "common/PrivilegedPlace.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>
#include <sstream>

namespace {

using namespace mw::place;

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool fileHas(const QString& path, const char* needle)
{
    return readFile(path).contains(needle);
}

[[maybe_unused]] bool samePath(const QString& a, const QString& b)
{
    return QString::compare(QDir::cleanPath(a), QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

[[maybe_unused]] bool backdate(const QString& path, int days)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    return f.setFileTime(QDateTime::currentDateTime().addDays(-days),
                         QFileDevice::FileModificationTime);
}

/// The logger's console echo, parked for the duration (it would bury the
/// suite's own output).
class MutedStdout
{
public:
    MutedStdout()
        : m_Saved(std::cout.rdbuf(m_Sink.rdbuf()))
    {}
    ~MutedStdout() { std::cout.rdbuf(m_Saved); }

private:
    std::ostringstream m_Sink;
    std::streambuf* m_Saved;
};

#ifdef Q_OS_WIN

bool makeDir(const QString& path, const QString& sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.toStdWString().c_str(),
                                                              SDDL_REVISION_1, &sd, nullptr))
        return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const BOOL ok = CreateDirectoryW(QDir::toNativeSeparators(path).toStdWString().c_str(), &sa);
    LocalFree(sd);
    return ok != 0;
}

/// A junction needs no privilege, where a symbolic link does.
bool makeJunction(const QString& link, const QString& target)
{
    return QProcess::execute(QStringLiteral("cmd"),
                             {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                              QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)}) ==
           0;
}

bool makeHardLink(const QString& link, const QString& existing)
{
    return CreateHardLinkW(QDir::toNativeSeparators(link).toStdWString().c_str(),
                           QDir::toNativeSeparators(existing).toStdWString().c_str(), nullptr) != 0;
}

bool elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
    TOKEN_ELEVATION e{};
    DWORD got = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got);
    CloseHandle(token);
    return !ok || e.TokenIsElevated != 0;
}

/// Whether this process's own token is granted @p access on a directory with
/// the security descriptor @p sddl: 1 granted, 0 refused, -1 could not be asked.
int granted(const QString& sddl, DWORD access)
{
    // AccessCheck wants a primary group; the directories are made without one.
    QString full = sddl;
    full.replace(QStringLiteral("D:P"), QStringLiteral("G:S-1-5-18D:P"));
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(full.toStdWString().c_str(),
                                                              SDDL_REVISION_1, &sd, nullptr))
        return -1;
    int result = -1;
    HANDLE process = nullptr;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process) &&
        DuplicateToken(process, SecurityImpersonation, &token)) {
        GENERIC_MAPPING map{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                            FILE_ALL_ACCESS};
        PRIVILEGE_SET privileges{};
        DWORD size = sizeof(privileges);
        DWORD got = 0;
        BOOL ok = FALSE;
        if (AccessCheck(sd, token, access, &map, &privileges, &size, &got, &ok))
            result = ok ? 1 : 0;
    }
    if (token) CloseHandle(token);
    if (process) CloseHandle(process);
    LocalFree(sd);
    return result;
}

void run_windows(const QString& tmp, const QString& me)
{
    using win::Check;
    const QStringList trusted{me};
    // Every right for this account, inherited by what is made inside, and
    // nothing from the directory above.
    const QString mine = QStringLiteral("O:%1D:P(A;OICI;FA;;;%1)").arg(me);

    // ── The descriptors the real directories are made with ──────────────────
    {
        const QStringList real = win::defaultTrustedSids();
        CHECK_EQ(real, QStringList({"S-1-5-32-544", "S-1-5-18"}));
        CHECK_EQ(win::lockedSddl(win::Dir::HostData, real),
                 QString("O:S-1-5-32-544D:P(A;OICI;FA;;;S-1-5-32-544)(A;OICI;FA;;;S-1-5-18)"
                         "(A;;FRFX;;;AU)"));
        CHECK_EQ(win::lockedSddl(win::Dir::UserLogs, real, "S-1-5-21-1-2-3-1001"),
                 QString("O:S-1-5-32-544D:P(A;OICI;FA;;;S-1-5-32-544)(A;OICI;FA;;;S-1-5-18)"
                         "(A;OICI;FRFX;;;S-1-5-21-1-2-3-1001)"));
        CHECK_EQ(win::lockedSddl(win::Dir::Crashes, real),
                 QString("O:S-1-5-32-544D:P(A;OICI;FA;;;S-1-5-32-544)(A;OICI;FA;;;S-1-5-18)"));

        // What an account without administrator rights is granted on them:
        // its own logs to read, the two directories above to walk through,
        // and nothing that adds, deletes, renames or changes access. Asked of
        // this process's own token, so only when it is not elevated.
        if (!elevated()) {
            const QString logs = win::lockedSddl(win::Dir::UserLogs, real, me);
            const QString above = win::lockedSddl(win::Dir::HostData, real);
            const QString crashes = win::lockedSddl(win::Dir::Crashes, real);
            CHECK_EQ(granted(logs, FILE_LIST_DIRECTORY | FILE_TRAVERSE), 1);
            CHECK_EQ(granted(above, FILE_LIST_DIRECTORY | FILE_TRAVERSE), 1);
            CHECK_EQ(granted(crashes, FILE_LIST_DIRECTORY), 0);
            for (const QString& sddl : {logs, above, crashes}) {
                for (DWORD access :
                     {DWORD(FILE_ADD_FILE), DWORD(FILE_ADD_SUBDIRECTORY), DWORD(FILE_DELETE_CHILD),
                      DWORD(DELETE), DWORD(WRITE_DAC), DWORD(WRITE_OWNER),
                      DWORD(FILE_WRITE_ATTRIBUTES), DWORD(FILE_WRITE_EA)})
                    CHECK_EQ(granted(sddl, access), 0);
            }
        } else {
            std::fprintf(stderr, "  (elevated run: the access checks are skipped)\n");
        }
    }

    // ── What a directory is accepted for ────────────────────────────────────
    const QString shapes = tmp + QStringLiteral("/shapes");
    CHECK(makeDir(shapes, mine));
    {
        const QString good = shapes + QStringLiteral("/good");
        CHECK(makeDir(good, mine));
        CHECK(win::checkLockedDir(good, trusted) == Check::Ok);
        CHECK(win::checkLockedDir(shapes + QStringLiteral("/absent"), trusted) == Check::Missing);

        // Read access for another account is fine; so is a listing right.
        const QString read = shapes + QStringLiteral("/read");
        CHECK(makeDir(read, mine + QStringLiteral("(A;OICI;FRFX;;;BU)(A;;FRFX;;;AU)")));
        CHECK(win::checkLockedDir(read, trusted) == Check::Ok);

        // A junction, and a directory reached through one.
        const QString junction = shapes + QStringLiteral("/junction");
        CHECK(makeJunction(junction, good));
        CHECK(win::checkLockedDir(junction, trusted) == Check::ReparsePoint);
        const QString inner = good + QStringLiteral("/inner");
        CHECK(makeDir(inner, mine));
        CHECK(win::checkLockedDir(inner, trusted) == Check::Ok);
        CHECK(win::checkLockedDir(junction + QStringLiteral("/inner"), trusted) ==
              Check::ReparsePoint);

        // A file is not a directory.
        const QString file = shapes + QStringLiteral("/file");
        CHECK(writeFile(file, "x"));
        CHECK(win::checkLockedDir(file, trusted) == Check::NotDirectory);

        // Another account may write, add or delete: each one alone refuses.
        int n = 0;
        for (const char* ace :
             {"(A;OICI;FW;;;BU)", "(A;;0x2;;;BU)", "(A;;0x4;;;AU)", "(A;;0x40;;;BU)",
              "(A;;SD;;;BU)", "(A;;WD;;;BU)", "(A;;WO;;;BU)", "(A;;0x100;;;BU)", "(A;;0x10;;;BU)",
              "(A;OICIIO;GA;;;BU)", "(A;;FA;;;WD)"}) {
            const QString dir = shapes + QStringLiteral("/write-%1").arg(n++);
            CHECK(makeDir(dir, mine + QLatin1String(ace)));
            CHECK(win::checkLockedDir(dir, trusted) == Check::Acl);
        }
        // Anything but a plain allow entry refuses, a deny included.
        const QString deny = shapes + QStringLiteral("/deny");
        CHECK(makeDir(deny, QStringLiteral("O:%1D:P(D;;0x2;;;BU)(A;OICI;FA;;;%1)").arg(me)));
        CHECK(win::checkLockedDir(deny, trusted) == Check::Acl);
        // A DACL that takes what the directory above hands down.
        const QString inheriting = shapes + QStringLiteral("/inheriting");
        CHECK(makeDir(inheriting, QStringLiteral("O:%1D:(A;OICI;FA;;;%1)").arg(me)));
        CHECK(win::checkLockedDir(inheriting, trusted) == Check::Acl);
        // Owned by an account that is not one of the trusted ones.
        CHECK(win::checkLockedDir(good, {QStringLiteral("S-1-5-18")}) == Check::Owner);
    }

    // ── The install directory ───────────────────────────────────────────────
    {
        QString resolved;
        const QString app = shapes + QStringLiteral("/app");
        // Not protected, read and run for everyone: how an installer leaves it.
        CHECK(makeDir(app, QStringLiteral("O:%1D:(A;OICI;FA;;;%1)(A;OICI;FRFX;;;BU)").arg(me)));
        CHECK(win::checkInstallDir(app, trusted, &resolved) == Check::Ok);
        CHECK(samePath(resolved, app));
        // An entry that only applies to what is made inside is not the
        // directory's own.
        const QString creator = shapes + QStringLiteral("/app-creator");
        CHECK(makeDir(creator, QStringLiteral("O:%1D:(A;OICI;FA;;;%1)(A;OICIIO;GA;;;CO)").arg(me)));
        CHECK(win::checkInstallDir(creator, trusted) == Check::Ok);
        // Another account may add to it, or delete it.
        const QString adds = shapes + QStringLiteral("/app-adds");
        CHECK(makeDir(adds, QStringLiteral("O:%1D:(A;OICI;FA;;;%1)(A;;0x4;;;AU)").arg(me)));
        CHECK(win::checkInstallDir(adds, trusted) == Check::Acl);
        const QString deletes = shapes + QStringLiteral("/app-deletes");
        CHECK(makeDir(deletes, QStringLiteral("O:%1D:(A;OICI;FA;;;%1)(A;;SD;;;BU)").arg(me)));
        CHECK(win::checkInstallDir(deletes, trusted) == Check::Acl);
        // Owned by an account that is neither trusted nor the system's.
        CHECK(win::checkInstallDir(app, {}) == Check::Owner);
        CHECK(win::checkInstallDir(shapes + QStringLiteral("/absent"), trusted) == Check::Missing);
    }

    // ── A delete never goes through a junction ──────────────────────────────
    const QString elsewhere = tmp + QStringLiteral("/elsewhere");
    CHECK(makeDir(elsewhere, mine));
    CHECK(writeFile(elsewhere + QStringLiteral("/keep.txt"), "kept\n"));
    {
        const QString tree = tmp + QStringLiteral("/tree");
        CHECK(QDir().mkpath(tree + QStringLiteral("/a/b")));
        CHECK(writeFile(tree + QStringLiteral("/a/one.txt"), "1"));
        CHECK(writeFile(tree + QStringLiteral("/a/b/two.txt"), "2"));
        CHECK(writeFile(tree + QStringLiteral("/a/b/locked.txt"), "3"));
        CHECK(QFile::setPermissions(tree + QStringLiteral("/a/b/locked.txt"),
                                    QFileDevice::ReadOwner));
        CHECK(makeJunction(tree + QStringLiteral("/a/out"), elsewhere));
        CHECK(removeNoFollow(tree));
        CHECK(!QFileInfo::exists(tree));
        CHECK(fileHas(elsewhere + QStringLiteral("/keep.txt"), "kept"));
        CHECK(removeNoFollow(tree)); // nothing there: done
    }

    // ── The directories a privileged process makes, and uses ────────────────
    const QString app = tmp + QStringLiteral("/app");
    CHECK(makeDir(app, mine));
    setForTests(app, trusted);
    CHECK(isPrivileged());
    const QString hostData = app + QStringLiteral("/host-data");
    const QString logs = logDir();
    CHECK(samePath(logs, hostData + QStringLiteral("/logs/") + me));
    CHECK(win::checkLockedDir(hostData, trusted) == Check::Ok);
    CHECK(win::checkLockedDir(hostData + QStringLiteral("/logs"), trusted) == Check::Ok);
    CHECK(win::checkLockedDir(logs, trusted) == Check::Ok);
    const QString crashes = crashDir();
    CHECK(samePath(crashes, hostData + QStringLiteral("/crashes")));
    CHECK(win::checkLockedDir(crashes, trusted) == Check::Ok);
    CHECK_EQ(logDir(), logs); // there already: the same one
    CHECK(isWritablePlace(logs));
    CHECK(isWritablePlace(crashes));
    CHECK(!isWritablePlace(app));
    CHECK(!isWritablePlace(elsewhere));
    CHECK(!isWritablePlace(tmp));
    // A reader named on the command line: a SID, written as the system writes it.
    CHECK(samePath(logDir(QStringLiteral("S-1-5-21-1-2-3-1001")),
                   hostData + QStringLiteral("/logs/S-1-5-21-1-2-3-1001")));
    CHECK(logDir(QStringLiteral("..")).isEmpty());
    // An account, not a group or a well-known SID.
    for (const char* sid : {"S-1-1-0", "S-1-5-11", "S-1-5-32-545", "S-1-5-32-544", "S-1-5-18",
                            "S-1-5-19", "S-1-15-2-1"})
        CHECK(logDir(QLatin1String(sid)).isEmpty());
    CHECK(samePath(logDir(QStringLiteral("S-1-12-1-1-2-3-4")),
                   hostData + QStringLiteral("/logs/S-1-12-1-1-2-3-4")));
    CHECK(logDir(QStringLiteral("s-1-5-21-1-2-3-1001")).isEmpty());
    CHECK(logDir(QStringLiteral("S-1-5-21-1-2-3-1001\\x")).isEmpty());

    // A directory that is not what it must be is made again, and what it
    // pointed to is left alone.
    {
        CHECK(removeNoFollow(hostData));
        CHECK(makeJunction(hostData, elsewhere));
        const QString again = logDir();
        CHECK(samePath(again, logs));
        CHECK(win::checkLockedDir(hostData, trusted) == Check::Ok);
        CHECK(fileHas(elsewhere + QStringLiteral("/keep.txt"), "kept"));
        CHECK(!QFileInfo::exists(elsewhere + QStringLiteral("/logs")));

        // One another account may write to, with something in it.
        CHECK(removeNoFollow(hostData));
        CHECK(makeDir(hostData, mine + QStringLiteral("(A;OICI;FW;;;BU)")));
        CHECK(writeFile(hostData + QStringLiteral("/planted.txt"), "x"));
        CHECK(samePath(crashDir(), crashes));
        CHECK(win::checkLockedDir(hostData, trusted) == Check::Ok);
        CHECK(!QFileInfo::exists(hostData + QStringLiteral("/planted.txt")));
        CHECK(samePath(logDir(), logs));
    }

    // An install directory that fails its check: no directory, no file.
    {
        const QString open = tmp + QStringLiteral("/open-app");
        CHECK(makeDir(open, mine + QStringLiteral("(A;;0x4;;;AU)")));
        setForTests(open, trusted);
        CHECK(logDir().isEmpty());
        CHECK(crashDir().isEmpty());
        CHECK(readableLogDirs().isEmpty());
        CHECK(!QFileInfo::exists(open + QStringLiteral("/host-data")));
        setForTests(app, trusted);
    }

    // ── The log, opened in a privileged process ─────────────────────────────
    {
        QFile plain(logs + QStringLiteral("/plain.log"));
        QString why;
        CHECK(openForAppend(plain, &why));
        CHECK(why.isEmpty());
        CHECK(plain.write("first\n") == 6);
        plain.close();
        QFile second(logs + QStringLiteral("/plain.log"));
        CHECK(openForAppend(second));
        CHECK(second.write("second\n") == 7);
        CHECK(second.size() > 12);
        second.close();
        const QByteArray text = readFile(logs + QStringLiteral("/plain.log"));
        CHECK(text.contains("first") && text.contains("second"));
        CHECK(text.indexOf("first") < text.indexOf("second")); // appended

        // A second name for a file somewhere else.
        const QString outside = tmp + QStringLiteral("/outside.txt");
        CHECK(writeFile(outside, "untouched\n"));
        CHECK(makeHardLink(logs + QStringLiteral("/linked.log"), outside));
        QFile linked(logs + QStringLiteral("/linked.log"));
        CHECK(!openForAppend(linked, &why));
        CHECK(why.contains(QStringLiteral("more than one link")));
        CHECK(!linked.isOpen());
        CHECK_EQ(readFile(outside), QByteArray("untouched\n"));
        CHECK(QFile::remove(logs + QStringLiteral("/linked.log")));

        // A directory where the file should be.
        CHECK(QDir().mkdir(logs + QStringLiteral("/dir.log")));
        QFile dir(logs + QStringLiteral("/dir.log"));
        CHECK(!openForAppend(dir, &why));
        CHECK(QDir().rmdir(logs + QStringLiteral("/dir.log")));

        // A directory that is not one made for this process's files: the
        // install directory itself, one beside it, one reached by a junction.
        QFile inApp(app + QStringLiteral("/stray.log"));
        CHECK(!openForAppend(inApp, &why));
        CHECK(!QFileInfo::exists(app + QStringLiteral("/stray.log")));
        QFile inTmp(tmp + QStringLiteral("/stray.log"));
        CHECK(!openForAppend(inTmp, &why));
        CHECK(!QFileInfo::exists(tmp + QStringLiteral("/stray.log")));
        CHECK(makeJunction(logs + QStringLiteral("/sub"), elsewhere));
        QFile through(logs + QStringLiteral("/sub/through.log"));
        CHECK(!openForAppend(through, &why));
        CHECK(!QFileInfo::exists(elsewhere + QStringLiteral("/through.log")));
        CHECK(removeNoFollow(logs + QStringLiteral("/sub")));
        CHECK(fileHas(elsewhere + QStringLiteral("/keep.txt"), "kept"));
    }

    // ── The logger, in a privileged process ─────────────────────────────────
    {
        Logger* log = Logger::instance();
        log->setBudgetForTests(8 * 1024, 64 * 1024, 3600);
        log->setReopenRetryForTests(0);

        // A log that rolls: each generation is opened the same way.
        const QString live = logs + QStringLiteral("/moonlightweb.log");
        log->setLogFile(live);
        CHECK(log->hasLogFile());
        CHECK_EQ(log->logFilePath(), live);
        Logger::info(QStringLiteral("place-marker"));
        CHECK(fileHas(live, "place-marker"));
        const QString filler(200, QLatin1Char('x'));
        for (int i = 0; i < 200; ++i)
            Logger::info(filler);
        CHECK(log->hasLogFile());
        CHECK_EQ(log->logFilePath(), live);
        const QStringList archives =
            QDir(logs).entryList({QStringLiteral("moonlightweb.log.*")}, QDir::Files);
        CHECK(!archives.isEmpty());
        CHECK(QFileInfo(live).size() < 16 * 1024);
        Logger::info(QStringLiteral("after-roll"));
        CHECK(fileHas(live, "after-roll"));

        // A second name for a file somewhere else, where the log should be:
        // nothing is written through it, and the log comes back once the name
        // is free.
        const QString outside = tmp + QStringLiteral("/outside.txt");
        const QString linked = logs + QStringLiteral("/moonlightweb-probe.log");
        CHECK(makeHardLink(linked, outside));
        log->setLogFile(linked);
        CHECK(!log->hasLogFile());
        Logger::info(QStringLiteral("must-not-land"));
        Logger::info(QStringLiteral("must-not-land"));
        CHECK_EQ(readFile(outside), QByteArray("untouched\n"));
        CHECK(QFile::remove(linked));
        Logger::info(QStringLiteral("landed"));
        CHECK(log->hasLogFile());
        CHECK(fileHas(linked, "landed"));
        CHECK(fileHas(linked, "2 lines lost"));
        CHECK_EQ(readFile(outside), QByteArray("untouched\n"));

        // A path outside the directories: no file, and nothing rolled or
        // dropped beside it.
        const QString stray = tmp + QStringLiteral("/stray/moonlightweb.log");
        CHECK(QDir().mkpath(tmp + QStringLiteral("/stray")));
        CHECK(writeFile(stray + QStringLiteral(".20200101-000000"), "an old archive\n"));
        CHECK(backdate(stray + QStringLiteral(".20200101-000000"), 30));
        CHECK(writeFile(stray, QByteArray(32 * 1024, 'x')));
        log->setLogFile(stray);
        CHECK(!log->hasLogFile());
        Logger::info(QStringLiteral("must-not-land"));
        CHECK_EQ(QFileInfo(stray).size(), qint64(32 * 1024));
        CHECK(QFileInfo::exists(stray + QStringLiteral(".20200101-000000")));
        CHECK_EQ(QDir(tmp + QStringLiteral("/stray")).entryList(QDir::Files).size(), 2);

        // The bench's traces go beside the log, or nowhere.
        CHECK(samePath(traceDir(live), logs));
        CHECK(traceDir(stray).isEmpty());
        CHECK(traceDir(QString()).isEmpty());

        log->setBudgetForTests(0, 0, 0);
        log->setReopenRetryForTests(-1);
    }

    // ── A worker's own files, a week on ─────────────────────────────────────
    {
        const QStringList old{"moonlightweb-worker-11.log", "click-trace-11-1700000000000.csv",
                              "relay-frames-11-1700000000000.csv",
                              "relay-audio-11-1700000000000.csv"};
        const QStringList kept{"moonlightweb-worker-22.log", "moonlightweb.log.20200101-000000",
                               "moonlightweb-vdisplay.log", "notes.csv"};
        for (const QString& name : old + kept)
            CHECK(writeFile(logs + QLatin1Char('/') + name, "x\n"));
        for (const QString& name : old)
            CHECK(backdate(logs + QLatin1Char('/') + name, 8));
        for (const QString& name : kept.mid(1))
            CHECK(backdate(logs + QLatin1Char('/') + name, 8));
        pruneWorkerFiles(logs);
        for (const QString& name : old)
            CHECK(!QFileInfo::exists(logs + QLatin1Char('/') + name));
        for (const QString& name : kept)
            CHECK(QFileInfo::exists(logs + QLatin1Char('/') + name));
        // Not a directory of this process's: nothing is dropped from it.
        CHECK(writeFile(tmp + QStringLiteral("/stray/moonlightweb-worker-11.log"), "x\n"));
        CHECK(backdate(tmp + QStringLiteral("/stray/moonlightweb-worker-11.log"), 8));
        pruneWorkerFiles(tmp + QStringLiteral("/stray"));
        CHECK(QFileInfo::exists(tmp + QStringLiteral("/stray/moonlightweb-worker-11.log")));
    }

    // ── What the archive of logs reads ──────────────────────────────────────
    {
        // A privileged process: its own directory under host-data, alone.
        CHECK_EQ(readableLogDirs().size(), 1);
        CHECK(samePath(readableLogDirs().value(0), logs));

        // A process with its user's rights: its own directory, then its
        // account's under host-data when it is there and checks out.
        const QString own = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/logs");
        setForTests(app, trusted, /*privileged=*/false);
        CHECK(!isPrivileged());
        CHECK_EQ(readableLogDirs().size(), 2);
        CHECK(samePath(readableLogDirs().value(0), own));
        CHECK(samePath(readableLogDirs().value(1), logs));
        // It opens its log as it always has, wherever that is.
        QFile anywhere(tmp + QStringLiteral("/stray/user.log"));
        CHECK(openForAppend(anywhere));
        anywhere.close();
        CHECK(isWritablePlace(tmp));
        CHECK(samePath(traceDir(QString()), QDir::tempPath()));

        // Not there, or not what it must be: its own directory alone.
        setForTests(app, trusted);
        CHECK(removeNoFollow(logs));
        CHECK(makeDir(logs, mine + QStringLiteral("(A;OICI;FW;;;BU)")));
        setForTests(app, trusted, /*privileged=*/false);
        CHECK_EQ(readableLogDirs().size(), 1);
        CHECK(samePath(readableLogDirs().value(0), own));
        setForTests(app, trusted);
        CHECK(removeNoFollow(logs));
        setForTests(app, trusted, /*privileged=*/false);
        CHECK_EQ(readableLogDirs().size(), 1);
        setForTests(app, trusted);
        CHECK(samePath(logDir(), logs)); // made again for what follows
    }

    // ── The uninstaller's verb ──────────────────────────────────────────────
    {
        CHECK(writeFile(crashes + QStringLiteral("/crash-1.dmp"), "x"));
        CHECK(writeFile(logs + QStringLiteral("/moonlightweb.log"), "x"));
        CHECK_EQ(removeHostData(/*withLogs=*/false), 0);
        CHECK(!QFileInfo::exists(crashes));
        CHECK(QFileInfo::exists(logs + QStringLiteral("/moonlightweb.log")));
        CHECK_EQ(removeHostData(/*withLogs=*/false), 0); // nothing left to delete
        CHECK(makeJunction(hostData + QStringLiteral("/crashes"), elsewhere));
        CHECK_EQ(removeHostData(/*withLogs=*/true), 0);
        CHECK(!QFileInfo::exists(hostData));
        CHECK(fileHas(elsewhere + QStringLiteral("/keep.txt"), "kept"));

        // An install directory that is a junction to another one, which holds
        // a host-data of its own: nothing is deleted there.
        const QString other = tmp + QStringLiteral("/other-app");
        CHECK(makeDir(other, mine));
        CHECK(QDir().mkpath(other + QStringLiteral("/host-data/crashes")));
        CHECK(QDir().mkpath(other + QStringLiteral("/host-data/logs/") + me));
        CHECK(writeFile(other + QStringLiteral("/host-data/crashes/crash-1.dmp"), "x"));
        CHECK(writeFile(other + QStringLiteral("/host-data/logs/") + me +
                            QStringLiteral("/moonlightweb.log"),
                        "x"));
        const QString through = tmp + QStringLiteral("/app-link");
        CHECK(makeJunction(through, other));
        setForTests(through, trusted);
        CHECK_EQ(removeHostData(/*withLogs=*/false), 0);
        CHECK_EQ(removeHostData(/*withLogs=*/true), 0);
        CHECK(QFileInfo::exists(other + QStringLiteral("/host-data/crashes/crash-1.dmp")));
        CHECK(QFileInfo::exists(other + QStringLiteral("/host-data/logs/") + me +
                                QStringLiteral("/moonlightweb.log")));

        // An install directory that fails its check: the same.
        const QString open = tmp + QStringLiteral("/open-app");
        CHECK(QDir().mkpath(open + QStringLiteral("/host-data/crashes")));
        CHECK(writeFile(open + QStringLiteral("/host-data/crashes/crash-1.dmp"), "x"));
        setForTests(open, trusted);
        CHECK_EQ(removeHostData(/*withLogs=*/true), 0);
        CHECK(QFileInfo::exists(open + QStringLiteral("/host-data/crashes/crash-1.dmp")));
        setForTests(app, trusted);
    }
}

#else // POSIX

void run_posix(const QString& tmp)
{
    // This account stands in for root: the chain is checked from `base` down.
    const QString base = tmp + QStringLiteral("/data");
    CHECK(QDir().mkpath(base));
    CHECK(chmod(QFile::encodeName(base).constData(), 0700) == 0);
    const QStringList trusted{QString::number(getuid())};
    setForTests(base, trusted);
    CHECK(isPrivileged());

    const QString canon = QFileInfo(base).canonicalFilePath();
    const QString logs = logDir();
    CHECK_EQ(logs, canon + QStringLiteral("/logs"));
    CHECK(QFileInfo(logs).isDir());
    CHECK_EQ(crashDir(), canon + QStringLiteral("/crashes"));
    CHECK(isWritablePlace(logs));
    CHECK(!isWritablePlace(tmp));
    CHECK_EQ(readableLogDirs(), QStringList{logs});

    // A regular file with one name, of this account's.
    QString why;
    QFile plain(logs + QStringLiteral("/plain.log"));
    CHECK(openForAppend(plain, &why));
    CHECK(plain.write("first\n") == 6);
    plain.close();
    CHECK(fileHas(logs + QStringLiteral("/plain.log"), "first"));

    const QString outside = tmp + QStringLiteral("/outside.txt");
    CHECK(writeFile(outside, "untouched\n"));
    CHECK(QFile::link(outside, logs + QStringLiteral("/symlinked.log")));
    QFile symlinked(logs + QStringLiteral("/symlinked.log"));
    CHECK(!openForAppend(symlinked, &why));
    CHECK(link(QFile::encodeName(outside).constData(),
               QFile::encodeName(logs + QStringLiteral("/linked.log")).constData()) == 0);
    QFile linked(logs + QStringLiteral("/linked.log"));
    CHECK(!openForAppend(linked, &why));
    CHECK_EQ(readFile(outside), QByteArray("untouched\n"));

    // Not under the checked directory.
    QFile stray(tmp + QStringLiteral("/stray.log"));
    CHECK(!openForAppend(stray, &why));
    CHECK(!QFileInfo::exists(tmp + QStringLiteral("/stray.log")));
    CHECK(traceDir(QString()).isEmpty());
    CHECK_EQ(traceDir(logs + QStringLiteral("/plain.log")), logs);

    // A directory others may write to, anywhere on the chain: no file.
    CHECK(chmod(QFile::encodeName(logs).constData(), 0777) == 0);
    CHECK(!isWritablePlace(logs));
    QFile refused(logs + QStringLiteral("/refused.log"));
    CHECK(!openForAppend(refused, &why));
    CHECK(!QFileInfo::exists(logs + QStringLiteral("/refused.log")));
    CHECK(logDir().isEmpty());
    CHECK(chmod(QFile::encodeName(logs).constData(), 0700) == 0);
    CHECK_EQ(logDir(), logs);
    CHECK(chmod(QFile::encodeName(base).constData(), 0775) == 0);
    CHECK(logDir().isEmpty());
    CHECK(chmod(QFile::encodeName(base).constData(), 0700) == 0);

    // A link that leads out of the checked directory.
    const QString elsewhere = tmp + QStringLiteral("/elsewhere");
    CHECK(QDir().mkpath(elsewhere));
    CHECK(QDir(crashDir()).removeRecursively());
    CHECK(QFile::link(elsewhere, base + QStringLiteral("/crashes")));
    CHECK(crashDir().isEmpty());
    CHECK(QDir(elsewhere).isEmpty());
    CHECK(QFile::remove(base + QStringLiteral("/crashes")));

    // A process with its user's rights: as it always has.
    setForTests(base, trusted, /*privileged=*/false);
    CHECK(!isPrivileged());
    QFile anywhere(tmp + QStringLiteral("/user.log"));
    CHECK(openForAppend(anywhere));
    anywhere.close();
    CHECK(isWritablePlace(tmp));
    CHECK_EQ(readableLogDirs().size(), 1);
}

#endif

} // namespace

void run_privileged_place_tests()
{
    SECTION("PrivilegedPlace");
    MutedStdout quiet;

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString tmp = QFileInfo(dir.path()).canonicalFilePath();

#ifdef Q_OS_WIN
    const QString me = currentUserSid();
    CHECK(me.startsWith(QStringLiteral("S-1-")));
    if (!me.isEmpty()) run_windows(tmp, me);
#else
    CHECK(currentUserSid().isEmpty());
    run_posix(tmp);
#endif

    // Back to what the whole suite runs as (tests/main.cpp): a process with
    // its user's own rights, whatever account runs it.
    setForTests(QString(), {}, /*privileged=*/false);
    // Leave the logger somewhere harmless: QTemporaryDir cannot remove a file
    // this process still holds open.
    Logger::instance()->setLogFile(QDir::tempPath() + QStringLiteral("/mw-backend-tnr.log"),
                                   /*rotating=*/false);
#ifdef Q_OS_WIN
    // The directories made with a DACL of their own go the same way they would
    // in an install.
    removeNoFollow(tmp + QStringLiteral("/shapes"));
#endif
}
