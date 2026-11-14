/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * `MoonlightWeb.exe --quit-server`, the installer's request that the running
 * server quit (server/QuitServer.h): the record read elevated without
 * creating, writing or following anything; the verb against a server of the
 * runner's own, in a second process; and the self-update's child, which no
 * owner ends (common/OwnedChild.h). Windows only.
 */
#include "test_framework.h"

#include <QtGlobal>

#ifdef Q_OS_WIN

#include "common/OwnedChild.h"
#include "network/RouterHoles.h"
#include "server/LocalChannel.h"
#include "server/QuitServer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#include <windows.h>
#include <shellapi.h>

namespace {

using mw::quitserver::Code;
using mw::quitserver::countRecordLines;

const QString kData = QStringLiteral("MoonlightWeb");

/// What is under @p root, with each entry's last write: equal before and
/// after means nothing was made, changed or removed there.
QStringList treeOf(const QString& root)
{
    QStringList out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        // Asked of the entry itself: what a listing shows comes from the
        // folder's index, which the file system brings up to date later.
        const QFileInfo info(it.filePath());
        out << info.absoluteFilePath() + QLatin1Char('|') +
                   QString::number(info.lastModified().toMSecsSinceEpoch()) + QLatin1Char('|') +
                   QString::number(info.size());
    }
    out.sort();
    return out;
}

QByteArray recordWith(int lines,
                      const QString& owner = QStringLiteral("0123456789abcdef0123456789abcdef"))
{
    mw::routerholes::Content content;
    content.owner = owner;
    content.tag = QStringLiteral("a3f9c1");
    const uint16_t ports[] = {3478, 3479, 3480, 3481};
    for (int i = 0; i < lines && i < 4; ++i) {
        mw::routerholes::Hole h;
        h.external = ports[i];
        h.internal = ports[i];
        h.kind = mw::routerholes::Kind::Tunnel;
        h.lan = QStringLiteral("192.168.1.20");
        content.holes.append(h);
    }
    return mw::routerholes::serialize(content);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(bytes) == bytes.size();
}

/// A junction (no privilege needed), made by the system's own tool.
bool junction(const QString& link, const QString& target)
{
    return QProcess::execute(QStringLiteral("cmd.exe"),
                             {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                              QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)}) ==
           0;
}

/// A server of this runner's own on @p endpoint, in a second process:
/// "exit" answers the quit then ends, "stay" answers and stays, "refuse"
/// answers 403. Ready once it says so.
struct Child
{
    QProcess process;

    bool start(const QString& endpoint, const QString& mode)
    {
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--quit-server-child"), endpoint, mode});
        if (!process.waitForStarted(5000)) return false;
        QElapsedTimer clock;
        clock.start();
        QByteArray said;
        while (clock.elapsed() < 10000) {
            process.waitForReadyRead(200);
            said += process.readAllStandardOutput();
            if (said.contains("ready")) return true;
            if (process.state() == QProcess::NotRunning) return false;
        }
        return false;
    }

    ~Child()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
};

QString endpointFor(const char* what)
{
    return QStringLiteral("\\\\.\\pipe\\mw-test-quit-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QLatin1String(what));
}

} // namespace

/// The second process of the verb's test: a server of the runner's own on
/// the channel it is told, as the installed server answers the quit.
int run_quit_server_child(int argc, char** argv)
{
    if (argc < 4) return 2;
    QCoreApplication app(argc, argv);
    const QString endpoint = QString::fromLocal8Bit(argv[2]);
    const QString mode = QString::fromLocal8Bit(argv[3]);
    mw::localchannel::Server server;
    QString why;
    const bool listening = server.listen(
        endpoint,
        [mode](const mw::localchannel::Request& request,
               mw::localchannel::Server::Respond respond) {
            if (request.path != QLatin1String("/api/system/quit") ||
                mode == QLatin1String("refuse")) {
                respond({403, QByteArrayLiteral("{}")});
                return;
            }
            respond({200, QByteArrayLiteral("{\"status\":\"quitting\"}")});
            if (mode == QLatin1String("exit"))
                QTimer::singleShot(300, qApp, []() { QCoreApplication::exit(0); });
        },
        &why);
    if (!listening) return 3;
    std::fputs("ready\n", stdout);
    std::fflush(stdout);
    return app.exec();
}

/// The second process of OwnedChild's test: it sleeps, then ends with a code.
int run_owned_child_sleep(int argc, char** argv)
{
    if (argc < 4) return 2;
    ::Sleep(static_cast<DWORD>(std::atoi(argv[2])));
    return std::atoi(argv[3]);
}

void run_quit_server_tests()
{
    SECTION("quit server: the verb is the one argument, nothing else");
    {
        char prog[] = "MoonlightWeb.exe";
        char verb[] = "--quit-server";
        char other[] = "--dev";
        char* alone[] = {prog, verb, nullptr};
        char* withMore[] = {prog, verb, other, nullptr};
        char* after[] = {prog, other, verb, nullptr};
        CHECK(mw::quitserver::isTheVerb(2, alone));
        CHECK(!mw::quitserver::isTheVerb(3, withMore));
        CHECK(!mw::quitserver::isTheVerb(3, after));
        CHECK(!mw::quitserver::isTheVerb(1, alone));
    }

    SECTION("quit server: no folder and no file are no line, and nothing is made");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        QDir().mkpath(roaming);
        const QStringList before = treeOf(root.path());
        const auto none = countRecordLines(roaming, kData);
        CHECK(none.read && none.lines == 0);
        QDir().mkpath(roaming + QStringLiteral("/MoonlightWeb/") + kData);
        const QStringList made = treeOf(root.path());
        const auto empty = countRecordLines(roaming, kData);
        CHECK(empty.read && empty.lines == 0);
        CHECK(treeOf(root.path()) == made);
        CHECK(before != made);
    }

    SECTION("quit server: a record is counted, and left as it was, lock-free");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        // Another install's record (its owner is not this one): counted only.
        const QByteArray bytes = recordWith(2);
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), bytes));
        const QStringList before = treeOf(root.path());
        const auto count = countRecordLines(roaming, kData);
        CHECK(count.read);
        CHECK(count.lines == 2);
        CHECK(treeOf(root.path()) == before);
        CHECK(!QFileInfo::exists(mine + QStringLiteral("/router_holes.lock")));
        QFile f(mine + QStringLiteral("/router_holes.json"));
        CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == bytes);
    }

    SECTION("quit server: a record that does not parse, or too large, is unreadable");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), QByteArrayLiteral("{ nope")));
        CHECK(!countRecordLines(roaming, kData).read);
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"),
                        QByteArray(static_cast<int>(mw::routerholes::kFileMax) + 1, ' ')));
        CHECK(!countRecordLines(roaming, kData).read);
    }

    SECTION("quit server: a junction on the way is refused, whatever it leads to");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString elsewhere = root.path() + QStringLiteral("/elsewhere");
        QDir().mkpath(elsewhere + QLatin1Char('/') + kData);
        CHECK(writeFile(elsewhere + QLatin1Char('/') + kData + QStringLiteral("/router_holes.json"),
                        recordWith(1)));
        QDir().mkpath(roaming);
        // The MoonlightWeb folder is a junction.
        CHECK(junction(roaming + QStringLiteral("/MoonlightWeb"), elsewhere));
        CHECK(!countRecordLines(roaming, kData).read);

        // The install's own folder is one.
        QTemporaryDir root2;
        const QString roaming2 = root2.path() + QStringLiteral("/Roaming");
        QDir().mkpath(roaming2 + QStringLiteral("/MoonlightWeb"));
        CHECK(junction(roaming2 + QStringLiteral("/MoonlightWeb/") + kData,
                       elsewhere + QLatin1Char('/') + kData));
        CHECK(!countRecordLines(roaming2, kData).read);
    }

    SECTION("quit server: a folder swapped for a junction once checked is caught");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        const QString elsewhere = root.path() + QStringLiteral("/elsewhere");
        QDir().mkpath(mine);
        QDir().mkpath(elsewhere);
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), recordWith(1)));
        CHECK(writeFile(elsewhere + QStringLiteral("/router_holes.json"), recordWith(3)));
        bool swapped = false;
        const auto count = countRecordLines(roaming, kData, [&]() {
            // Between the folder checks and the open: the folder goes, a
            // junction to another takes its name.
            swapped =
                QDir().rename(mine, mine + QStringLiteral(".moved")) && junction(mine, elsewhere);
        });
        CHECK(swapped);
        CHECK(!count.read);
        CHECK(count.lines == 0);
    }

    SECTION("quit server: nobody listening gives 2, or 6 with lines left");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        const mw::quitserver::Limits quick{500, 1000, 0};
        CHECK(mw::quitserver::run(endpointFor("nobody"), roaming, kData, quick) ==
              Code::NobodyToAsk);
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), recordWith(1)));
        CHECK(mw::quitserver::run(endpointFor("nobody"), roaming, kData, quick) ==
              Code::NobodyLinesLeft);
    }

    SECTION("quit server: a server that quits gives 0, or 1 with lines left");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        const mw::quitserver::Limits quick{3000, 5000, 0};
        {
            Child child;
            CHECK(child.start(endpointFor("exit0"), QStringLiteral("exit")));
            CHECK(mw::quitserver::run(endpointFor("exit0"), roaming, kData, quick) == Code::Gone);
            CHECK(child.process.state() == QProcess::NotRunning ||
                  child.process.waitForFinished(2000));
        }
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), recordWith(2)));
        {
            Child child;
            CHECK(child.start(endpointFor("exit1"), QStringLiteral("exit")));
            CHECK(mw::quitserver::run(endpointFor("exit1"), roaming, kData, quick) ==
                  Code::LinesLeft);
        }
    }

    SECTION("quit server: a server that stays gives 3 at the limit, and nothing is read");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), QByteArrayLiteral("{ nope")));
        Child child;
        CHECK(child.start(endpointFor("stay"), QStringLiteral("stay")));
        QElapsedTimer clock;
        clock.start();
        const mw::quitserver::Limits quick{3000, 600, 0};
        // The record would read as unreadable: 3 says it was not read.
        CHECK(mw::quitserver::run(endpointFor("stay"), roaming, kData, quick) ==
              Code::StillRunning);
        CHECK(clock.elapsed() < 5000);
        CHECK(child.process.state() != QProcess::NotRunning);
    }

    SECTION("quit server: a refusal gives 4");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        Child child;
        CHECK(child.start(endpointFor("refuse"), QStringLiteral("refuse")));
        const mw::quitserver::Limits quick{3000, 1000, 0};
        CHECK(mw::quitserver::run(endpointFor("refuse"), roaming, kData, quick) == Code::Refused);
    }

    SECTION("quit server: another account's server gives 7, and its record is not read");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        // A record that would read as unreadable: 7 says it was not read.
        CHECK(writeFile(mine + QStringLiteral("/router_holes.json"), QByteArrayLiteral("{ nope")));
        Child child;
        CHECK(child.start(endpointFor("other"), QStringLiteral("exit")));
        const mw::quitserver::Limits quick{3000, 5000, 0};
        bool asked = false;
        const auto otherAccount = [&asked](void* process) {
            asked = process != nullptr;
            return false;
        };
        CHECK(mw::quitserver::run(endpointFor("other"), roaming, kData, quick, nullptr,
                                  otherAccount) == Code::GoneOtherAccount);
        CHECK(asked);
    }

    SECTION("quit server: a process of this account is said to be one");
    {
        HANDLE self =
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ::GetCurrentProcessId());
        CHECK(self != nullptr);
        CHECK(mw::quitserver::sameAccount(self));
        if (self) ::CloseHandle(self);
        CHECK(!mw::quitserver::sameAccount(nullptr));
    }

    SECTION("quit server: a record that is a symbolic link is refused");
    {
        QTemporaryDir root;
        const QString roaming = root.path() + QStringLiteral("/Roaming");
        const QString mine = roaming + QStringLiteral("/MoonlightWeb/") + kData;
        QDir().mkpath(mine);
        const QString target = root.path() + QStringLiteral("/elsewhere.json");
        CHECK(writeFile(target, recordWith(1)));
        const std::wstring link =
            QDir::toNativeSeparators(mine + QStringLiteral("/router_holes.json")).toStdWString();
        const std::wstring to = QDir::toNativeSeparators(target).toStdWString();
        if (::CreateSymbolicLinkW(link.c_str(), to.c_str(),
                                  SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            const auto count = countRecordLines(roaming, kData);
            CHECK(!count.read);
            CHECK(count.lines == 0);
        } else {
            std::printf("  [SKIPPED] quit server: this account cannot make a symbolic link "
                        "(error %lu)\n",
                        ::GetLastError());
        }
    }

    SECTION("quit server: the self-update's command line is QProcess's");
    {
        const QString setup =
            QStringLiteral("C:\\Program Files\\MoonlightWeb\\update staging\\setup.exe");
        const QStringList args{QStringLiteral("/VERYSILENT"), QStringLiteral("/SUPPRESSMSGBOXES"),
                               QStringLiteral("/NORESTART"), QStringLiteral("/SP-"),
                               QStringLiteral("/LOG=C:\\Users\\a b\\update.log")};
        const QString line = mw::OwnedChild::commandLine(setup, args);
        CHECK(line ==
              QStringLiteral("\"C:\\Program Files\\MoonlightWeb\\update staging\\setup.exe\" "
                             " /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- "
                             "\"/LOG=C:\\Users\\a b\\update.log\""));
        CHECK(mw::OwnedChild::commandLine(QStringLiteral("C:/Windows/System32/schtasks.exe"),
                                          {QStringLiteral("/Run"), QStringLiteral("/TN"),
                                           QStringLiteral("MoonlightWeb Update")}) ==
              QStringLiteral(
                  "C:\\Windows\\System32\\schtasks.exe  /Run /TN \"MoonlightWeb Update\""));

        // Read back as Windows splits a command line, every argument is the one given.
        const QStringList hard{QStringLiteral(""), QStringLiteral("a\"b"),
                               QStringLiteral("C:\\dir with space\\"), QStringLiteral("x\\\\\"y"),
                               QStringLiteral("tab\there")};
        const QString all = mw::OwnedChild::commandLine(setup, args + hard);
        int n = 0;
        LPWSTR* split = ::CommandLineToArgvW(reinterpret_cast<LPCWSTR>(all.utf16()), &n);
        CHECK(split != nullptr);
        if (split) {
            const QStringList want = QStringList{setup} + args + hard;
            CHECK(n == want.size());
            for (int i = 0; i < n && i < want.size(); ++i)
                CHECK(QString::fromWCharArray(split[i]) == want.at(i));
            ::LocalFree(split);
        }
    }

    SECTION("quit server: the self-update's child is never ended by its owner");
    {
        auto* child = new mw::OwnedChild;
        CHECK(!child->start(QStringLiteral("run_tests.exe"), {}, {}));
        bool told = false;
        CHECK(child->start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("--owned-child-sleep"), QStringLiteral("1500"), QStringLiteral("7")},
            [&told](quint32) { told = true; }));
        HANDLE mine = nullptr;
        CHECK(::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(child->process()),
                                ::GetCurrentProcess(), &mine,
                                SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, 0));
        delete child;
        // Its owner gone, the child lives on, then ends with its own code.
        CHECK(::WaitForSingleObject(mine, 0) == WAIT_TIMEOUT);
        CHECK(::WaitForSingleObject(mine, 10000) == WAIT_OBJECT_0);
        DWORD code = 0;
        CHECK(::GetExitCodeProcess(mine, &code) && code == 7);
        ::CloseHandle(mine);
        CHECK(!told);
    }

    SECTION("quit server: while its owner lives, the child's end and code are told");
    {
        mw::OwnedChild child;
        quint32 code = 0;
        bool told = false;
        CHECK(child.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("--owned-child-sleep"), QStringLiteral("100"), QStringLiteral("5")},
            [&](quint32 c) {
                code = c;
                told = true;
            }));
        QElapsedTimer clock;
        clock.start();
        while (!told && clock.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        CHECK(told);
        CHECK(code == 5);
    }
}

#else

int run_quit_server_child(int, char**)
{
    return 2;
}

int run_owned_child_sleep(int, char**)
{
    return 2;
}

void run_quit_server_tests() {}

#endif
