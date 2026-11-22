/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The answer to a start that failed because a Mac's display was away
 * (DisplayAwayAnswer.h): the owner reads the reason, under a code; a guest of
 * a shared stream reads a neutral sentence, never that the Mac is locked
 * (22/11/2026: a locked screen saver kept taking the panel back, and the
 * viewer read "ScreenCaptureKit did not start within 5 s").
 */

#include "../native-host/src/platform/macos/DisplayAway.h"
#include "../src/streaming/DisplayAwayAnswer.h"

#include "test_framework.h"

#include <QJsonObject>
#include <QString>

namespace {

namespace away = mw::native::platform::away;

/// What the native session reports for @p s, as the worker hands it on.
QString nativeError(const away::AwayState& s)
{
    return QString::fromStdString(away::startError(s, "ScreenCaptureKit did not start within 5 s"));
}

} // namespace

void run_display_away_answer_tests()
{
    SECTION("DisplayAwayAnswer — one tag, spelt the same on both sides");
    {
        CHECK_EQ(QString::fromLatin1(DisplayAwayAnswer::kCode), QString::fromLatin1(away::kCode));
        CHECK(QString::fromLatin1(DisplayAwayAnswer::kGuestCode) !=
              QString::fromLatin1(DisplayAwayAnswer::kCode));
    }

    SECTION("DisplayAwayAnswer — any other failure answers as HttpResponse::error did");
    {
        const QJsonObject body = DisplayAwayAnswer::startFailure(
            QStringLiteral("ScreenCaptureKit did not start within 5 s"), 502);
        CHECK_EQ(body.value("error").toString(),
                 QStringLiteral("ScreenCaptureKit did not start within 5 s"));
        CHECK_EQ(body.value("status").toInt(), 502);
        CHECK(!body.contains("code"));
        CHECK_EQ(body.size(), 2);

        // A guest's answer is not touched either.
        QJsonObject guest = body;
        DisplayAwayAnswer::forGuest(guest);
        CHECK(guest == body);

        // Nor is another code's.
        QJsonObject other{{"error", "Host is busy"}, {"code", "app_running"}, {"status", 409}};
        const QJsonObject before = other;
        DisplayAwayAnswer::forGuest(other);
        CHECK(other == before);
    }

    SECTION("DisplayAwayAnswer — the owner reads the reason alone, the tag in \"code\"");
    {
        away::AwayState s;
        s.screenSaver = true;
        s.locked = true;
        const QJsonObject body = DisplayAwayAnswer::startFailure(nativeError(s), 502);
        CHECK_EQ(body.value("code").toString(), QStringLiteral("mac_display_away"));
        CHECK_EQ(body.value("status").toInt(), 502);
        const QString text = body.value("error").toString();
        CHECK(
            text.startsWith(QStringLiteral("the Mac's screen saver is on, and the Mac is locked")));
        CHECK(text.contains(QStringLiteral("wake and unlock it")));
        CHECK(text.contains(QStringLiteral("(ScreenCaptureKit did not start within 5 s)")));
        CHECK(!text.contains(QStringLiteral("mac_display_away")));
    }

    SECTION("DisplayAwayAnswer — a guest is never told the Mac is locked, nor why");
    {
        away::AwayState cases[7];
        cases[0].asleep = true;
        cases[1].screenSaver = true;
        cases[2].locked = true;
        cases[3].asleep = cases[3].screenSaver = true;
        cases[4].asleep = cases[4].locked = true;
        cases[5].screenSaver = cases[5].locked = true;
        cases[6].asleep = cases[6].screenSaver = cases[6].locked = true;
        for (const away::AwayState& s : cases) {
            QJsonObject body = DisplayAwayAnswer::startFailure(nativeError(s), 502);
            DisplayAwayAnswer::forGuest(body);
            CHECK_EQ(body.value("code").toString(), QStringLiteral("host_screen_unavailable"));
            CHECK_EQ(body.value("error").toString(),
                     QStringLiteral("The host's screen is not available right now."));
            CHECK_EQ(body.value("status").toInt(), 502);
            // Nothing of the reason is left anywhere in the answer.
            for (auto it = body.begin(); it != body.end(); ++it) {
                const QString v = it.value().toVariant().toString();
                CHECK(!v.contains(QStringLiteral("lock"), Qt::CaseInsensitive));
                CHECK(!v.contains(QStringLiteral("screen saver"), Qt::CaseInsensitive));
                CHECK(!v.contains(QStringLiteral("Mac"), Qt::CaseSensitive));
                CHECK(!v.contains(QStringLiteral("ScreenCaptureKit")));
            }
        }
    }

    SECTION("DisplayAwayAnswer — the owner's code: only the tag at the head counts");
    {
        // Quoted further in, or without its colon and space: no code for the
        // owner, the text left as it is.
        const QString quoted = QStringLiteral("relay: mac_display_away: x");
        CHECK(!DisplayAwayAnswer::startFailure(quoted, 502).contains("code"));
        CHECK_EQ(DisplayAwayAnswer::startFailure(quoted, 502).value("error").toString(), quoted);
        const QString bare = QStringLiteral("mac_display_away");
        CHECK(!DisplayAwayAnswer::startFailure(bare, 502).contains("code"));
        const QString noSpace = QStringLiteral("mac_display_away:x");
        CHECK(!DisplayAwayAnswer::startFailure(noSpace, 502).contains("code"));
    }

    SECTION("DisplayAwayAnswer — a guest's answer fails closed: the tag anywhere is enough");
    {
        away::AwayState s;
        s.screenSaver = true;
        s.locked = true;
        const QString full = nativeError(s); // "mac_display_away: the Mac's … locked …"

        // Still tagged, with no code: a body that did not go through
        // startFailure (the worker's non-object body, a plain
        // HttpResponse::error some day).
        QJsonObject untouched{{"error", full}, {"status", 502}};
        DisplayAwayAnswer::forGuest(untouched);
        CHECK_EQ(untouched.value("error").toString(),
                 QStringLiteral("The host's screen is not available right now."));
        CHECK_EQ(untouched.value("code").toString(), QStringLiteral("host_screen_unavailable"));
        CHECK_EQ(untouched.value("status").toInt(), 502);

        // The tag further in, behind someone else's prefix.
        QJsonObject wrapped{{"error", QStringLiteral("x ") + full}, {"status", 502}};
        DisplayAwayAnswer::forGuest(wrapped);
        CHECK_EQ(wrapped.value("error").toString(),
                 QStringLiteral("The host's screen is not available right now."));
        CHECK(!wrapped.value("error").toString().contains(QStringLiteral("lock")));

        // The worker's fallback shape for a body that was not JSON.
        QJsonObject raw{{"error", full}};
        DisplayAwayAnswer::forGuest(raw);
        CHECK(!raw.value("error").toString().contains(QStringLiteral("locked")));
        CHECK_EQ(raw.value("code").toString(), QStringLiteral("host_screen_unavailable"));
    }
}
