// A server restarting itself (server/ServerRestart.h) and the admin page's box
// of the shared ports applied by a restart (server/SharedPortsRestart.h).
#include "test_framework.h"
#include "server/ServerRestart.h"
#include "server/SharedPortsRestart.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QSet>

#include <chrono>
#include <cstdlib>
#include <thread>

/// The child of the "wait for the server it replaces" test: a process of this
/// very executable that lives argv[2] milliseconds.
int run_restart_linger_child(int argc, char** argv)
{
    if (argc < 3) return 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
    return 0;
}

namespace {

QStringList argsFor(const QSet<QString>& set, const QHash<QString, QString>& values = {})
{
    return mw::restart::relaunchArguments([&set](const QString& n) { return set.contains(n); },
                                          [&values](const QString& n) { return values.value(n); });
}

qint64 startLinger(QProcess& p, int ms)
{
    p.setProgram(QCoreApplication::applicationFilePath());
    p.setArguments({QStringLiteral("--restart-linger"), QString::number(ms)});
    p.start();
    if (!p.waitForStarted(5000)) return 0;
    return p.processId();
}

} // namespace

void run_server_restart_tests()
{
    SECTION("ServerRestart: a relaunched server gets the allowlist of its arguments, no more");
    {
        CHECK(argsFor({}).isEmpty());
        CHECK(argsFor({"autostart"}) == QStringList{"--autostart"});
        CHECK(argsFor({"dev", "debug", "verbose", "noadmin"}) ==
              (QStringList{"--noadmin", "--dev", "--debug", "--verbose"}));
        CHECK(argsFor({"port", "ws-port", "config", "log"}, {{"port", "18080"},
                                                             {"ws-port", "18001"},
                                                             {"config", "C:/x/settings.json"},
                                                             {"log", "C:/x/a.log"}}) ==
              (QStringList{"--port", "18080", "--ws-port", "18001", "--log", "C:/x/a.log",
                           "--config", "C:/x/settings.json"}));
        // One-off gestures and helpers' verbs are never repeated, whatever
        // they carry.
        const QSet<QString> gestures{"print-link",
                                     "open-link",
                                     "wait-link",
                                     "local-request",
                                     "status",
                                     "new-pin",
                                     "set-admin-password",
                                     "clear-admin-password",
                                     "enable-internet",
                                     "yes",
                                     "after-pid",
                                     "update-apply",
                                     "remove-host-data",
                                     "stream-worker",
                                     "owner-record",
                                     "quit-server",
                                     "max-allowed-bitrate"};
        CHECK(argsFor(gestures,
                      {{"local-request", "POST /api/admin/pin/generate"}, {"new-pin", "1234"}})
                  .isEmpty());
    }

    SECTION("ServerRestart: --after-pid takes decimal digits only");
    {
        qint64 pid = 0;
        CHECK(mw::restart::parsePid("4242", &pid) && pid == 4242);
        for (const char* bad :
             {"", "0", "-1", "+5", " 5", "5 ", "0x10", "1e3", "12a", "99999999999", "2147483648"})
            CHECK(!mw::restart::parsePid(QString::fromLatin1(bad), &pid));
        CHECK(mw::restart::parsePid("2147483647", &pid));
    }

    SECTION("ServerRestart: the copy waits for the server it replaces, and for no other");
    {
        // This process: nothing to wait for.
        CHECK(!mw::restart::waitForPredecessor(QCoreApplication::applicationPid(), 200));
        // A process of this executable that ends: waited for, until it is gone.
        {
            QProcess child;
            const qint64 pid = startLinger(child, 600);
            CHECK(pid > 0);
            QElapsedTimer clock;
            clock.start();
            CHECK(mw::restart::waitForPredecessor(pid, 10000));
            CHECK(clock.elapsed() >= 200);
            child.waitForFinished(5000);
        }
        // One that outlives the limit: the wait gives up, and says so.
        {
            QProcess child;
            const qint64 pid = startLinger(child, 4000);
            CHECK(pid > 0);
            QElapsedTimer clock;
            clock.start();
            CHECK(!mw::restart::waitForPredecessor(pid, 300));
            CHECK(clock.elapsed() < 3000);
            child.kill();
            child.waitForFinished(5000);
        }
        // Another program: never waited for.
        {
            QProcess other;
#ifdef _WIN32
            other.setProgram(QStringLiteral("cmd.exe"));
            other.setArguments({"/c", "ping -n 3 127.0.0.1 >nul"});
#else
            other.setProgram(QStringLiteral("sleep"));
            other.setArguments({"2"});
#endif
            other.start();
            CHECK(other.waitForStarted(5000));
            QElapsedTimer clock;
            clock.start();
            CHECK(!mw::restart::waitForPredecessor(other.processId(), 5000));
            CHECK(clock.elapsed() < 1000);
            other.kill();
            other.waitForFinished(5000);
        }
        // A process that has already gone: nothing to wait for.
        {
            QProcess child;
            const qint64 pid = startLinger(child, 10);
            CHECK(pid > 0);
            child.waitForFinished(5000);
            QElapsedTimer clock;
            clock.start();
            mw::restart::waitForPredecessor(pid, 2000);
            CHECK(clock.elapsed() < 1500);
        }
    }

    SECTION("SharedPortsRestart: the rule");
    {
        using S = SharedPortsRestart;
        // No wish, or the file is what runs: nothing, busy or not.
        for (const bool busy : {false, true}) {
            CHECK(S::decide(false, true, false, busy) == S::Step::Nothing);
            CHECK(S::decide(true, true, true, busy) == S::Step::Nothing);
            CHECK(S::decide(true, false, false, busy) == S::Step::Nothing);
        }
        // A wish the file still holds: wait while something would be cut.
        CHECK(S::decide(true, false, true, true) == S::Step::Wait);
        CHECK(S::decide(true, true, false, true) == S::Step::Wait);
        CHECK(S::decide(true, false, true, false) == S::Step::Restart);
        CHECK(S::decide(true, true, false, false) == S::Step::Restart);
    }

    SECTION("SharedPortsRestart: one restart for one change, after the streams, never again");
    {
        bool file = false, runs = true, busy = true;
        int restarts = 0;
        SharedPortsRestart r(SharedPortsRestart::Probes{[&file]() { return file; },
                                                        [&runs]() { return runs; },
                                                        [&busy]() { return busy; },
                                                        [&restarts]() {
                                                            ++restarts;
                                                            return true;
                                                        }},
                             20);
        // Busy (a stream, a pairing, an install, an update): it waits.
        r.asked();
        CHECK(r.pending() && restarts == 0);
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 100)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(r.pending() && restarts == 0);
        // The box ticked back meanwhile: the wish goes, nothing restarts.
        file = true;
        clock.restart();
        while (clock.elapsed() < 100)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(!r.pending() && restarts == 0);
        // Unticked again, and nothing runs any more: one restart, at once.
        file = false;
        busy = false;
        r.asked();
        CHECK(!r.pending() && restarts == 1);
        clock.restart();
        while (clock.elapsed() < 100)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(restarts == 1);
        // Asked while nothing differs (the new process, whatever its mux did):
        // no restart.
        runs = false;
        r.asked();
        CHECK(!r.pending() && restarts == 1);
        // Busy goes, then the streams start again before the next look: it
        // waits again, and restarts only once it is quiet.
        file = true;
        busy = true;
        r.asked();
        CHECK(r.pending() && restarts == 1);
        busy = false;
        clock.restart();
        while (clock.elapsed() < 200 && restarts == 1)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(!r.pending() && restarts == 2);
    }

    SECTION("SharedPortsRestart: a restart that could not start a copy is not tried in a loop");
    {
        int restarts = 0;
        SharedPortsRestart r(SharedPortsRestart::Probes{[]() { return false; },
                                                        []() { return true; },
                                                        []() { return false; },
                                                        [&restarts]() {
                                                            ++restarts;
                                                            return false;
                                                        }},
                             20);
        r.asked();
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 100)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(restarts == 1 && !r.pending());
    }
}
