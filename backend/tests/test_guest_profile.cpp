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
}
