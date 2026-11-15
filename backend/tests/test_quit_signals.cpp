/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * SIGTERM, SIGINT and SIGHUP end the server as its quit route does
 * (common/QuitSignals.h): in a second process that installs the handler as
 * the server does, a signal ends its event loop normally with code 0; one
 * inherited as ignored stays ignored; once the loop is over, a signal ends
 * the process at once; the pipe is never inherited. Unix only.
 */
#include "test_framework.h"

#include <QtGlobal>

#ifndef Q_OS_WIN

#include "common/QuitSignals.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

/// A second process of this runner: `--signal-child <mode>`.
struct Child
{
    QProcess process;
    QByteArray said;

    bool start(const char* mode)
    {
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--signal-child"), QString::fromLatin1(mode)});
        return process.waitForStarted(5000) && waitFor("ready", 10000);
    }

    bool waitFor(const char* word, int ms)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            said += process.readAllStandardOutput();
            if (said.contains(word)) return true;
            if (process.state() == QProcess::NotRunning) {
                said += process.readAllStandardOutput();
                return said.contains(word);
            }
            process.waitForReadyRead(100);
        }
        return false;
    }

    void send(int signal) { ::kill(static_cast<pid_t>(process.processId()), signal); }

    /// Ended by itself with that code (true), or by a signal (false), within @p ms.
    bool endedCleanly(int ms, int* code)
    {
        if (!process.waitForFinished(ms)) return false;
        said += process.readAllStandardOutput();
        *code = process.exitCode();
        return process.exitStatus() == QProcess::NormalExit;
    }

    ~Child()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
};

} // namespace

/// The second process: the handler installed as the server installs it.
///   plain        quits on a signal, then says "after-exec"
///   hup-ignored  SIGHUP ignored before install()
///   slow         after the loop, a hook that takes 3 s ("slow-hook")
///   elsewhere    the loop ended by QCoreApplication::exit from a timer, then the slow hook
///   fds          prints whether both ends of the pipe are close-on-exec, then quits
int run_quit_signals_child(int argc, char** argv)
{
    if (argc < 3) return 2;
    const QByteArray mode(argv[2]);
    if (mode == "hup-ignored") ::signal(SIGHUP, SIG_IGN);
    QCoreApplication app(argc, argv);
    const bool installed = mw::quitsignals::install(&app, [](int) {
        mw::beginQuit();
        mw::quitsignals::restoreDefaults();
        QCoreApplication::exit(0);
    });
    if (!installed) return 3;
    if (mode == "fds") {
        const bool r = ::fcntl(mw::quitsignals::readEnd(), F_GETFD) & FD_CLOEXEC;
        const bool w = ::fcntl(mw::quitsignals::writeEnd(), F_GETFD) & FD_CLOEXEC;
        std::printf("cloexec %d %d\n", r ? 1 : 0, w ? 1 : 0);
        QTimer::singleShot(0, &app, []() { QCoreApplication::exit(0); });
    }
    if (mode == "elsewhere") QTimer::singleShot(300, &app, []() { QCoreApplication::exit(0); });
    std::printf("ready\n");
    std::fflush(stdout);
    const int code = app.exec();
    mw::beginQuit();
    mw::quitsignals::restoreDefaults();
    std::printf("after-exec quitting=%d\n", mw::quitting() ? 1 : 0);
    std::fflush(stdout);
    if (mode == "slow" || mode == "elsewhere") {
        std::printf("slow-hook\n");
        std::fflush(stdout);
        ::sleep(3);
    }
    return code;
}

void run_quit_signals_tests()
{
    SECTION("quit signals: SIGTERM, SIGINT and SIGHUP end the loop, with code 0");
    {
        for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
            Child child;
            CHECK(child.start("plain"));
            child.send(signal);
            int code = -1;
            CHECK(child.endedCleanly(5000, &code));
            CHECK(code == 0);
            CHECK(child.said.contains("after-exec quitting=1"));
        }
    }

    SECTION("quit signals: a SIGHUP inherited as ignored stays ignored");
    {
        Child child;
        CHECK(child.start("hup-ignored"));
        child.send(SIGHUP);
        QThread::msleep(500);
        CHECK(child.process.state() != QProcess::NotRunning);
        CHECK(!child.said.contains("after-exec"));
        child.send(SIGTERM);
        int code = -1;
        CHECK(child.endedCleanly(5000, &code));
        CHECK(code == 0);
    }

    SECTION("quit signals: a second signal during the exit ends the process at once");
    {
        Child child;
        CHECK(child.start("slow"));
        child.send(SIGTERM);
        CHECK(child.waitFor("slow-hook", 5000));
        QElapsedTimer clock;
        clock.start();
        child.send(SIGTERM);
        int code = -1;
        CHECK(!child.endedCleanly(5000, &code));
        CHECK(child.process.state() == QProcess::NotRunning);
        CHECK(clock.elapsed() < 2500);
    }

    SECTION("quit signals: once the loop is over, whatever ended it, a signal ends the process");
    {
        Child child;
        CHECK(child.start("elsewhere"));
        CHECK(child.waitFor("slow-hook", 5000));
        QElapsedTimer clock;
        clock.start();
        child.send(SIGTERM);
        int code = -1;
        CHECK(!child.endedCleanly(5000, &code));
        CHECK(child.process.state() == QProcess::NotRunning);
        CHECK(clock.elapsed() < 2500);
    }

    SECTION("quit signals: both ends of the pipe are close-on-exec");
    {
        Child child;
        CHECK(child.start("fds"));
        int code = -1;
        CHECK(child.endedCleanly(5000, &code));
        CHECK(child.said.contains("cloexec 1 1"));
    }
}

#else

int run_quit_signals_child(int, char**)
{
    return 2;
}

void run_quit_signals_tests() {}

#endif
