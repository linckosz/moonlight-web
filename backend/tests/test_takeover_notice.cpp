/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * What a take-over notice says beyond "taken over", in a booth whose streams
 * have PINs (issue #68): how the stream was let go and by which machine, and
 * nothing else whatever the command line carried — never a PIN, never a
 * session, never a name that could draw on the viewer's page.
 */

#include "test_framework.h"
#include "../src/streaming/TakeoverNotice.h"

#include <QStringList>

void run_takeover_notice_tests()
{
    SECTION("TakeoverNotice — with its PIN, or after the idle time, by a machine");

    QJsonObject n = TakeoverNotice::withPin(QStringLiteral("LAPTOP-SAM"));
    CHECK(n.value("reason").toString() == QStringLiteral("pin"));
    CHECK(n.value("by").toString() == QStringLiteral("LAPTOP-SAM"));
    CHECK(!n.contains("idle_minutes"));
    CHECK_EQ(n.size(), 2);

    n = TakeoverNotice::afterIdle(QStringLiteral("LAPTOP-SAM"), 15);
    CHECK(n.value("reason").toString() == QStringLiteral("idle"));
    CHECK_EQ(n.value("idle_minutes").toInt(), 15);
    CHECK_EQ(n.size(), 3);

    SECTION("TakeoverNotice — only what a notice may carry");

    // Whatever else rode along on the command is dropped: the command name, a
    // PIN, a token, a field nobody named.
    const QJsonObject raw{{"cmd", "takenOver"}, {"reason", "pin"},       {"by", "PC"},
                          {"pin", "482913"},    {"booth_pin", "482913"}, {"token", "session-sam"},
                          {"idle_minutes", 15}, {"extra", true}};
    n = TakeoverNotice::clean(raw);
    CHECK(n.keys() == QStringList({"by", "reason"}));

    // No reason it knows: nothing, and the page shows the plain take-over.
    for (const char* reason : {"", "takeover", "PIN", "admin", "idle "})
        CHECK(TakeoverNotice::clean({{"reason", reason}, {"by", "PC"}}).isEmpty());
    CHECK(TakeoverNotice::clean({{"by", "PC"}}).isEmpty());
    CHECK(TakeoverNotice::clean({{"reason", 1}}).isEmpty());

    // Idle minutes only for an idle take-over, and only as the key allows them.
    CHECK(!TakeoverNotice::afterIdle("PC", 0).contains("idle_minutes"));
    CHECK(!TakeoverNotice::afterIdle("PC", -5).contains("idle_minutes"));
    CHECK(!TakeoverNotice::afterIdle("PC", 241).contains("idle_minutes"));
    CHECK_EQ(TakeoverNotice::afterIdle("PC", 240).value("idle_minutes").toInt(), 240);
    CHECK(!TakeoverNotice::clean({{"reason", "idle"}, {"idle_minutes", "15"}})
               .contains("idle_minutes"));

    SECTION("TakeoverNotice — a machine name as the page may show it");

    // Control and format characters go, the rest is trimmed and cut.
    CHECK(TakeoverNotice::cleanName(QStringLiteral("  LAPTOP-SAM \n")) ==
          QStringLiteral("LAPTOP-SAM"));
    CHECK(TakeoverNotice::cleanName(QString::fromUtf8("A\x07"
                                                      "B\x1b[31m\xe2\x80\xae"
                                                      "C")) == QStringLiteral("AB[31mC"));
    CHECK(TakeoverNotice::cleanName(QString(200, QLatin1Char('x'))).size() ==
          TakeoverNotice::kMaxNameLength);
    CHECK(TakeoverNotice::cleanName(QString::fromUtf8("Poste de Léo")) ==
          QString::fromUtf8("Poste de Léo"));
    // No name at all: the reason alone.
    n = TakeoverNotice::withPin(QStringLiteral(" \t"));
    CHECK(!n.contains("by"));
    CHECK(n.value("reason").toString() == QStringLiteral("pin"));
    // Not a string: no name.
    CHECK(!TakeoverNotice::clean({{"reason", "pin"}, {"by", 42}}).contains("by"));
}
