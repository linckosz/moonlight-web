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

#include "test_framework.h"
#include "AutostartEntry.h"

#include <QString>

using namespace Autostart::entry;

// The Linux login item is rewritten at every start when it says something else
// than this version would write. A user who started the app through
// `env MW_LAN_ONLY=1 …` — a host kept off the Internet — lost that at the first
// start, and the next login ran without it. What is kept, and what is not.
void run_autostart_entry_tests()
{
    SECTION("Autostart entry: the user's env prefix");

    const QString launcher = QStringLiteral("\"/opt/moonlightweb-dev/bin/moonlightweb-launch\"");

    // The case that asked for it, as the UM790Pro's entry reads.
    CHECK_EQ(
        envPrefix(QStringLiteral("env MW_LAN_ONLY=1 ") + launcher + QStringLiteral(" --autostart")),
        QStringLiteral("env MW_LAN_ONLY=1 "));

    // Several, env by its path, the spacing as written.
    CHECK_EQ(envPrefix(QStringLiteral("env A=1 B_2=two ") + launcher),
             QStringLiteral("env A=1 B_2=two "));
    CHECK_EQ(envPrefix(QStringLiteral("/usr/bin/env MW_LAN_ONLY=1 /opt/x --autostart")),
             QStringLiteral("/usr/bin/env MW_LAN_ONLY=1 "));
    CHECK_EQ(envPrefix(QStringLiteral("env  A=1   /opt/x")), QStringLiteral("env  A=1   "));
    CHECK_EQ(envPrefix(QStringLiteral("\"/usr/bin/env\" A=1 /opt/x")),
             QStringLiteral("\"/usr/bin/env\" A=1 "));

    // Quoted arguments, with both escaping levels of the spec: a value with a
    // space, an escaped quote (\\" in the file), a program path with a space.
    CHECK_EQ(envPrefix(QStringLiteral("env \"GREETING=a b\" /opt/x")),
             QStringLiteral("env \"GREETING=a b\" "));
    CHECK_EQ(envPrefix(QStringLiteral("env \"A=say \\\\\"hi\\\\\"\" /opt/x")),
             QStringLiteral("env \"A=say \\\\\"hi\\\\\"\" "));
    CHECK_EQ(envPrefix(QStringLiteral("env A=1 \"/opt/My App/launch\" --autostart")),
             QStringLiteral("env A=1 "));
    // \s is a space once the string escapes are undone: it separates.
    CHECK_EQ(envPrefix(QStringLiteral("env\\sA=1 /opt/x")), QStringLiteral("env\\sA=1 "));

    // Kept only when it is env, then assignments, then a program.
    CHECK_EQ(envPrefix(launcher + QStringLiteral(" --autostart")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env ") + launcher), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env A=1")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env A=1 B=2")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("myenv A=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("environment A=1 /opt/x")), QString());
    // env itself only, not a program that took its name somewhere else.
    CHECK_EQ(envPrefix(QStringLiteral("/bin/env A=1 /opt/x")), QStringLiteral("/bin/env A=1 "));
    CHECK_EQ(envPrefix(QStringLiteral("/home/u/bin/env A=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("./env A=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QString()), QString());

    // Not a variable name: a shell would not take it, env would run it.
    CHECK_EQ(envPrefix(QStringLiteral("env 1A=2 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env =x /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env A-B=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env A1=1 /opt/x")), QStringLiteral("env A1=1 "));

    // env's own options are not read: rewritten as before.
    CHECK_EQ(envPrefix(QStringLiteral("env -u FOO A=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env -i A=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env -S \"A=1 /opt/x\" /opt/y")), QString());

    // A value the spec does not allow is left alone.
    CHECK_EQ(envPrefix(QStringLiteral("env \"A=1 /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env A=\"1\" /opt/x")), QString());
    CHECK_EQ(envPrefix(QStringLiteral("env \"A=1\"x /opt/x")), QString());

    // Put back in front of what this version writes, it reads back the same:
    // the next start finds the entry right and leaves it alone.
    const QString written =
        launcher + QStringLiteral(" --autostart --dev --config \"/home/u/My Settings.json\"");
    CHECK_EQ(envPrefix(QStringLiteral("env MW_LAN_ONLY=1 ") + written),
             QStringLiteral("env MW_LAN_ONLY=1 "));
    CHECK_EQ(envPrefix(envPrefix(QStringLiteral("env A=1 B=2 ") + written) + written),
             QStringLiteral("env A=1 B=2 "));

    SECTION("Autostart entry: the Exec= line");

    const QString entry = QStringLiteral("[Desktop Entry]\n"
                                         "Type=Application\n"
                                         "Name=MoonlightWebDev\n"
                                         "TryExec=/opt/x\n"
                                         "Exec=env MW_LAN_ONLY=1 \"/opt/x\" --autostart\n"
                                         "Icon=moonlightweb-dev\n");
    CHECK_EQ(execValue(entry), QStringLiteral("env MW_LAN_ONLY=1 \"/opt/x\" --autostart"));
    QString crlf = entry;
    crlf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    CHECK_EQ(execValue(crlf), QStringLiteral("env MW_LAN_ONLY=1 \"/opt/x\" --autostart"));
    CHECK_EQ(execValue(QStringLiteral("[Desktop Entry]\nName=x\n")), QString());
    CHECK_EQ(execValue(QString()), QString());
    CHECK_EQ(envPrefix(execValue(entry)), QStringLiteral("env MW_LAN_ONLY=1 "));
}
