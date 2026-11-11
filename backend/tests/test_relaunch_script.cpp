/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The shell sequence a macOS/Linux self-update leaves behind: install, stop the
 * old server, start the new one. The start is automatic, so it must carry
 * --autostart — without it, an updated host opened its admin page in a browser
 * on its own desktop (v0.4.0 acceptance bench, 12/10/2026).
 */
#include "test_framework.h"
#include "network/RelaunchScript.h"

void run_relaunch_script_tests()
{
    SECTION("RelaunchScript");

    using relaunchscript::build;
    using relaunchscript::shQuote;

    CHECK(shQuote(QStringLiteral("/opt/a b")) == QStringLiteral("'/opt/a b'"));
    CHECK(shQuote(QStringLiteral("it's")) == QStringLiteral("'it'\\''s'"));

    // The relaunch is an automatic start, after the install and the kill.
    const QString plain = build(QStringLiteral("apt-get install -y x.deb"), 1234,
                                QStringLiteral("/opt/moonlightweb/bin/moonlightweb-launch"), {});
    CHECK(plain == QStringLiteral("apt-get install -y x.deb; kill 1234 2>/dev/null; sleep 3; "
                                  "'/opt/moonlightweb/bin/moonlightweb-launch' --autostart "
                                  ">/dev/null 2>&1 &"));

    // The settings file it ran on comes back with it, quoted.
    const QString config = build(QStringLiteral("true"), 7, QStringLiteral("/tmp/MW.AppImage"),
                                 QStringLiteral("/home/u/my mw.json"));
    CHECK(config.contains(
        QStringLiteral("'/tmp/MW.AppImage' --autostart --config '/home/u/my mw.json' >")));

    // A "%1" in a path is text, not a placeholder for the next argument.
    const QString percent = build(QStringLiteral("mv %1"), 9, QStringLiteral("/x/%1"), {});
    CHECK(percent.startsWith(QStringLiteral("mv %1; kill 9 ")));
    CHECK(percent.contains(QStringLiteral("'/x/%1' --autostart >")));

    // Under its LaunchAgent the server comes back through launchd, never by a
    // direct start that would leave it outside the job. No -k: a job launchd
    // already restarted after the kill is left alone.
    const QString agent = build(QStringLiteral("rsync a b"), 42, QStringLiteral("/A.app/x"), {},
                                QStringLiteral("gui/501/com.moonlightweb.agent"));
    CHECK(agent == QStringLiteral("rsync a b; kill 42 2>/dev/null; sleep 3; "
                                  "/bin/launchctl kickstart 'gui/501/com.moonlightweb.agent' "
                                  ">/dev/null 2>&1"));
    CHECK(!agent.contains(QStringLiteral("--autostart")));

    // With its plist at hand, the job is loaded anew after the swap instead:
    // launchd's record of the old binary would get the new one killed at
    // launch. A failed load still brings the server back, directly.
    const QString reload =
        build(QStringLiteral("rsync a b"), 42, QStringLiteral("/A.app/x"), {},
              QStringLiteral("gui/501/com.moonlightweb.agent"),
              QStringLiteral("/Users/l o/Library/LaunchAgents/com.moonlightweb.agent.plist"));
    CHECK(reload ==
          QStringLiteral("rsync a b; kill 42 2>/dev/null; sleep 3; "
                         "/bin/launchctl bootout 'gui/501/com.moonlightweb.agent' >/dev/null 2>&1; "
                         "/bin/launchctl bootstrap 'gui/501' "
                         "'/Users/l o/Library/LaunchAgents/com.moonlightweb.agent.plist' "
                         ">/dev/null 2>&1 || { '/A.app/x' --autostart >/dev/null 2>&1 & }"));
    CHECK(!reload.contains(QStringLiteral("kickstart")));

    // The DEV edition's agent is a domain like any other.
    const QString dev =
        build(QStringLiteral("true"), 1, QStringLiteral("/D.app/x"), {},
              QStringLiteral("gui/502/com.moonlightweb.agent.dev"), QStringLiteral("/p.plist"));
    CHECK(dev.contains(QStringLiteral("bootout 'gui/502/com.moonlightweb.agent.dev'")));
    CHECK(dev.contains(QStringLiteral("bootstrap 'gui/502' '/p.plist'")));
}
