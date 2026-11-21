/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * An invited player's picture (GuestProfile.h, issue #67).
 *
 * What matters: Auto lays the host's picture in the guest's screen and takes
 * the tallest rung that fits, whichever way the screen is held; a TV gets 720;
 * no screen, or one the page could not have measured, is the old default; a
 * join can name a rung or Auto and nothing else; and the bitrate is the owner's
 * Auto rule to the kbps. The table is the one the page's rule is held to
 * (frontend/test/fixtures/guest-profile-cases.json), so the two cannot drift.
 */

#include "../src/server/GuestProfile.h"

#include "test_framework.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>

using namespace mw::guestprofile;

namespace {

double ratioOf(const QString& aspect)
{
    const QStringList parts = aspect.split(QLatin1Char(':'));
    const double w = parts.value(0).toDouble(), h = parts.value(1).toDouble();
    return w > 0 && h > 0 ? w / h : 0.0;
}

QJsonObject loadCases()
{
    QFile file(QStringLiteral(FRONTEND_DIR "test/fixtures/guest-profile-cases.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

void run_guest_profile_tests()
{
    const QJsonObject cases = loadCases();

    SECTION("GuestProfile — the shared table is there");
    CHECK(cases.value(QStringLiteral("height")).toArray().size() >= 10);
    CHECK(cases.value(QStringLiteral("bitrate")).toArray().size() >= 10);

    SECTION("GuestProfile — Auto's height, from the shared table");
    for (const QJsonValue& v : cases.value(QStringLiteral("height")).toArray()) {
        const QJsonObject c = v.toObject();
        QJsonObject body{{QStringLiteral("auto"), true}, {QStringLiteral("tv"), c.value("tv")}};
        if (c.value("screen").isString()) body.insert(QStringLiteral("screen"), c.value("screen"));
        const int got = autoHeight(parseScreen(body), ratioOf(c.value("hostAspect").toString()),
                                   requestFromTv(body));
        const int want = c.value("height").toInt();
        if (got != want)
            std::fprintf(stderr, "  screen %s, host %s, tv %d: %d, not %d\n",
                         qPrintable(c.value("screen").toString(QStringLiteral("none"))),
                         qPrintable(c.value("hostAspect").toString()), c.value("tv").toBool(), got,
                         want);
        CHECK_EQ(got, want);
    }

    SECTION("GuestProfile — the Auto bitrate, from the shared table");
    for (const QJsonValue& v : cases.value(QStringLiteral("bitrate")).toArray()) {
        const QJsonObject c = v.toObject();
        const int got =
            autoBitrateKbps(c.value("height").toInt(), ratioOf(c.value("aspect").toString()),
                            c.value("lan").toBool());
        const int want = c.value("kbps").toInt();
        if (got != want)
            std::fprintf(stderr, "  %dp %s lan %d: %d kbps, not %d\n", c.value("height").toInt(),
                         qPrintable(c.value("aspect").toString()), c.value("lan").toBool(), got,
                         want);
        CHECK_EQ(got, want);
    }

    SECTION("GuestProfile — a join names Auto or a rung, nothing else");
    CHECK_EQ(requestedRung(QJsonObject{{"auto", true}}), 0);
    CHECK_EQ(requestedRung(QJsonObject{{"auto", true}, {"height", 1440}}), 0);
    CHECK_EQ(requestedRung(QJsonObject{{"height", 720}}), 720);
    CHECK_EQ(requestedRung(QJsonObject{{"height", 1080}}), 1080);
    CHECK_EQ(requestedRung(QJsonObject{{"height", 1440}}), 1440);
    CHECK_EQ(requestedRung(QJsonObject{{"height", 2160}}), kDefaultHeight);
    CHECK_EQ(requestedRung(QJsonObject{{"height", -5}}), kDefaultHeight);
    CHECK_EQ(requestedRung(QJsonObject{{"height", "1440"}}), kDefaultHeight);
    CHECK_EQ(requestedRung(QJsonObject{{"auto", "yes"}}), kDefaultHeight);
    CHECK_EQ(requestedRung(QJsonObject{}), kDefaultHeight);

    SECTION("GuestProfile — only a screen the page could measure is read");
    CHECK(parseScreen(QJsonObject{{"screen", "1920x1080"}}).has_value());
    CHECK(parseScreen(QJsonObject{{"screen", "320x8192"}}).has_value());
    CHECK(!parseScreen(QJsonObject{{"screen", "319x1080"}}).has_value());
    CHECK(!parseScreen(QJsonObject{{"screen", "1920x8193"}}).has_value());
    CHECK(!parseScreen(QJsonObject{{"screen", "1920x1080x3"}}).has_value());
    CHECK(!parseScreen(QJsonObject{{"screen", "-1920x1080"}}).has_value());
    CHECK(!parseScreen(QJsonObject{{"screen", "99999999999x1080"}}).has_value());
    CHECK(!parseScreen(QJsonObject{{"screen", 1920}}).has_value());
    CHECK(!parseScreen(QJsonObject{}).has_value());
    if (const auto s = parseScreen(QJsonObject{{"screen", "2532x1170"}})) {
        CHECK_EQ(s->width, 2532);
        CHECK_EQ(s->height, 1170);
    }

    SECTION("GuestProfile — a host's shape nobody knows is 16:9");
    CHECK_EQ(autoHeight(Screen{1920, 1080}, 0.0, false), 1080);
    CHECK_EQ(autoHeight(Screen{1920, 1080}, -1.0, false), 1080);
    CHECK_EQ(autoBitrateKbps(1080, 0.0, false), autoBitrateKbps(1080, 16.0 / 9.0, false));
    CHECK_EQ(autoBitrateKbps(0, 16.0 / 9.0, true), autoBitrateKbps(1080, 16.0 / 9.0, true));

    SECTION("GuestProfile — a host taller than wide is laid along the long side");
    // 9:16 at 1440 lines is 810 wide: it fits 1080×1920 held either way.
    CHECK_EQ(autoHeight(Screen{1080, 1920}, 9.0 / 16.0, false), 1440);
    CHECK_EQ(autoHeight(Screen{1920, 1080}, 9.0 / 16.0, false), 1440);
    CHECK_EQ(autoHeight(Screen{720, 1280}, 9.0 / 16.0, false), 1080);

    SECTION("GuestProfile — Auto is never taller than the host's screen");
    CHECK_EQ(capToHost(1440, 1080), 1080);
    CHECK_EQ(capToHost(1440, 1200), 1080);
    CHECK_EQ(capToHost(1440, 1440), 1440);
    CHECK_EQ(capToHost(1080, 2160), 1080);
    CHECK_EQ(capToHost(1080, 600), 720);
    CHECK_EQ(capToHost(1440, 0), 1440);
    CHECK_EQ(capToHost(1440, -1), 1440);

    SECTION("GuestProfile — the host's screen is the display captured (GUESTD1)");
    // The N95 of 21/11/2026: its 1080p screen captured, a 1440p virtual
    // display beside it, a guest on a 1440p screen. Auto is 1080, not 1440.
    CHECK_EQ(shownScreenHeight(1080, {1440, 1080}), 1080);
    CHECK_EQ(
        capToHost(autoHeight(parseScreen(QJsonObject{{"screen", "2560x1440"}}), 16.0 / 9.0, false),
                  shownScreenHeight(1080, {1440, 1080})),
        1080);
    // The native host lists no modes: the display captured alone holds it.
    CHECK_EQ(shownScreenHeight(1080, {}), 1080);
    // No display known (a GameStream host): its tallest mode, whatever their order.
    CHECK_EQ(shownScreenHeight(0, {1080, 2160, 1440}), 2160);
    CHECK_EQ(shownScreenHeight(-1, {1080}), 1080);
    // Neither: unknown, and capToHost leaves Auto as it is.
    CHECK_EQ(shownScreenHeight(0, {}), 0);

    SECTION("GuestProfile — the ladder's steps, from the shared table");
    CHECK(cases.value(QStringLiteral("degrade")).toArray().size() >= 10);
    for (const QJsonValue& v : cases.value(QStringLiteral("degrade")).toArray()) {
        const QJsonObject c = v.toObject();
        const Picture base{c.value("height").toInt(), c.value("kbps").toInt()};
        const Picture got = degraded(base, ratioOf(c.value("aspect").toString()),
                                     c.value("lan").toBool(), c.value("n").toInt());
        const QJsonArray want = c.value("want").toArray();
        if (got.height != want.at(0).toInt() || got.bitrateKbps != want.at(1).toInt())
            std::fprintf(stderr, "  %dp %d kbps lan %d step %d: %dp %d kbps, not %dp %d kbps\n",
                         base.height, base.bitrateKbps, c.value("lan").toBool(),
                         c.value("n").toInt(), got.height, got.bitrateKbps, want.at(0).toInt(),
                         want.at(1).toInt());
        CHECK_EQ(got.height, want.at(0).toInt());
        CHECK_EQ(got.bitrateKbps, want.at(1).toInt());
    }

    SECTION("GuestProfile — a step is only ever an integer from 0 to 8");
    CHECK(cases.value(QStringLiteral("degradeRead")).toArray().size() >= 10);
    for (const QJsonValue& v : cases.value(QStringLiteral("degradeRead")).toArray()) {
        const QJsonObject c = v.toObject();
        const QJsonObject body = c.value("body").toObject();
        const int got = readDegrade(body);
        if (got != c.value("n").toInt())
            std::fprintf(stderr, "  %s: %d, not %d\n",
                         QJsonDocument(body).toJson(QJsonDocument::Compact).constData(), got,
                         c.value("n").toInt());
        CHECK_EQ(got, c.value("n").toInt());
    }
    CHECK(asksLadderStep(QJsonObject{{"degrade", 0}}));
    CHECK(asksLadderStep(QJsonObject{{"degrade", 8}}));
    CHECK(asksLadderStep(QJsonObject{{"degrade", 1000000}}));
    // Anything readDegrade would not read as a step is an ordinary join.
    CHECK(!asksLadderStep(QJsonObject{{"degrade", "x"}}));
    CHECK(!asksLadderStep(QJsonObject{{"degrade", "3"}}));
    CHECK(!asksLadderStep(QJsonObject{{"degrade", -1}}));
    CHECK(!asksLadderStep(QJsonObject{{"degrade", 2.5}}));
    CHECK(!asksLadderStep(QJsonObject{{"degrade", true}}));
    CHECK(!asksLadderStep(QJsonObject{{"degrade", QJsonValue::Null}}));
    CHECK(!asksLadderStep(QJsonObject{{"auto", true}}));

    SECTION("GuestProfile — the ladder only lowers, whatever the step");
    for (const int h : kRungs)
        for (const bool lan : {false, true}) {
            const Picture base{h, autoBitrateKbps(h, 16.0 / 9.0, lan)};
            Picture prev = base;
            for (int n = 0; n <= kDegradeMax + 3; ++n) {
                const Picture p = degraded(base, 16.0 / 9.0, lan, n);
                CHECK(p.height <= prev.height);
                CHECK(p.bitrateKbps <= prev.bitrateKbps);
                CHECK(p.height >= kDegradeFloorHeight);
                CHECK(p.bitrateKbps >= kDegradeFloorKbps);
                prev = p;
            }
            CHECK(degraded(base, 16.0 / 9.0, lan, -4) == base);
        }

    SECTION("GuestProfile — the shared feed in Auto: the tallest connected guest's");
    {
        const FeedTarget at1080{1080, true, 0};
        const FeedGuest small{1080, true, true};
        const FeedGuest big{1440, true, true};
        // Joined, never connected: it raises nothing (G1).
        const FeedGuest bigJoining{1440, true, false};
        const FeedGuest farAway{1080, false, true};

        FeedTarget t = feedAutoTarget(at1080, {small, big}, 0, false);
        CHECK_EQ(t.height, 1440);
        CHECK(t.lan);
        CHECK_EQ(t.counted, 2);

        t = feedAutoTarget(at1080, {small, bigJoining}, 0, false);
        CHECK_EQ(t.height, 1080);
        CHECK_EQ(t.counted, 1);

        // Never above the host's screen (G3).
        t = feedAutoTarget(at1080, {big}, 1200, false);
        CHECK_EQ(t.height, 1080);

        // Never down while guests watch (I67-baisse B): the big one left.
        t = feedAutoTarget(FeedTarget{1440, true, 0}, {small}, 0, false);
        CHECK_EQ(t.height, 1440);

        // One guest away from the LAN: the Internet rule, and it stays.
        t = feedAutoTarget(at1080, {small, farAway}, 0, false);
        CHECK(!t.lan);
        t = feedAutoTarget(FeedTarget{1080, false, 0}, {small}, 0, false);
        CHECK(!t.lan);

        // Nobody connected: as it is.
        t = feedAutoTarget(FeedTarget{720, false, 0}, {bigJoining}, 0, false);
        CHECK_EQ(t.height, 720);
        CHECK(!t.lan);
        CHECK_EQ(t.counted, 0);

        // The owner has just picked Auto: the guests' own, lower included.
        t = feedAutoTarget(FeedTarget{1440, false, 0}, {small}, 0, true);
        CHECK_EQ(t.height, 1080);
        CHECK(t.lan);
    }
}
