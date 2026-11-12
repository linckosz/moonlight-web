/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The pen (issue #50): the page's 'pen' message as the relay reads it, and the
 * pen as the host was told about it when the watchdog lifts it under a stroke.
 */
#include "test_framework.h"
#include "streaming/InputMessageCodec.h"
#include "streaming/PenInput.h"

#include <QJsonObject>

#include <cmath>
#include <limits>

namespace {

PenEvent read(const QJsonObject& msg, bool* ok = nullptr)
{
    PenEvent e;
    const bool good = InputMsg::readPen(msg, e);
    if (ok) *ok = good;
    return e;
}

PenEvent event(uint8_t type, float x = 0.5f, float y = 0.5f, uint8_t tool = Pen::kToolPen)
{
    PenEvent e;
    e.eventType = type;
    e.toolType = tool;
    e.x = x;
    e.y = y;
    e.pressureOrDistance = type == Pen::kDown || type == Pen::kMove ? 0.5f : 0.0f;
    return e;
}

} // namespace

void run_input_pen_tests()
{
    SECTION("Pen input");

    // ── The page's message, read ────────────────────────────────────────────
    {
        bool ok = false;
        const PenEvent e = read(QJsonObject{{"type", "pen"},
                                            {"event", "move"},
                                            {"tool", "eraser"},
                                            {"buttons", 1},
                                            {"x", 0.25},
                                            {"y", 0.75},
                                            {"pressure", 0.6},
                                            {"tilt", 30},
                                            {"rotation", 90}},
                                &ok);
        CHECK(ok);
        CHECK_EQ(e.eventType, Pen::kMove);
        CHECK_EQ(e.toolType, Pen::kToolEraser);
        CHECK_EQ(e.buttons, uint8_t(1));
        CHECK(std::fabs(e.x - 0.25f) < 1e-6f);
        CHECK(std::fabs(e.y - 0.75f) < 1e-6f);
        CHECK(std::fabs(e.pressureOrDistance - 0.6f) < 1e-6f);
        CHECK_EQ(e.tilt, uint8_t(30));
        CHECK_EQ(e.rotation, uint16_t(90));
    }

    // Each event the page sends, and nothing else.
    {
        bool ok = false;
        CHECK_EQ(read(QJsonObject{{"event", "hover"}}, &ok).eventType, Pen::kHover);
        CHECK(ok);
        CHECK_EQ(read(QJsonObject{{"event", "down"}}).eventType, Pen::kDown);
        CHECK_EQ(read(QJsonObject{{"event", "up"}}).eventType, Pen::kUp);
        CHECK_EQ(read(QJsonObject{{"event", "leave"}}).eventType, Pen::kHoverLeave);
        read(QJsonObject{{"event", "cancel"}}, &ok);
        CHECK(!ok);
        read(QJsonObject{{"event", 3}}, &ok);
        CHECK(!ok);
        read(QJsonObject{}, &ok);
        CHECK(!ok);
    }

    // Held to the packet's ranges, whatever the page sent.
    {
        const PenEvent e = read(QJsonObject{{"event", "move"},
                                            {"tool", "brush"},
                                            {"buttons", 0xFF},
                                            {"x", -3.0},
                                            {"y", 7.0},
                                            {"pressure", 2.0},
                                            {"tilt", 400},
                                            {"rotation", -10}});
        CHECK_EQ(e.toolType, Pen::kToolUnknown);
        CHECK_EQ(e.buttons, Pen::kButtonsMask);
        CHECK_EQ(e.x, 0.0f);
        CHECK_EQ(e.y, 1.0f);
        CHECK_EQ(e.pressureOrDistance, 1.0f);
        CHECK_EQ(e.tilt, uint8_t(90));
        CHECK_EQ(e.rotation, uint16_t(350));

        const PenEvent wrap =
            read(QJsonObject{{"event", "move"}, {"tilt", 1e300}, {"rotation", 719.6}});
        CHECK_EQ(wrap.tilt, uint8_t(90));
        CHECK_EQ(wrap.rotation, uint16_t(0));

        const PenEvent junk =
            read(QJsonObject{{"event", "move"}, {"x", "left"}, {"pressure", true}});
        CHECK_EQ(junk.x, 0.0f);
        CHECK_EQ(junk.pressureOrDistance, 0.0f);
    }

    // A tilt is unknown unless the page measured both of its angles.
    {
        const PenEvent none = read(QJsonObject{{"event", "move"}});
        CHECK_EQ(none.tilt, Pen::kTiltUnknown);
        CHECK_EQ(none.rotation, Pen::kRotationUnknown);
        const PenEvent half = read(QJsonObject{{"event", "move"}, {"tilt", 30}});
        CHECK_EQ(half.tilt, Pen::kTiltUnknown);
        CHECK_EQ(half.rotation, Pen::kRotationUnknown);
    }

    // Under the mouse's rights: a viewer without the mouse has no pen either.
    {
        InputMsg::Policy viewer;
        viewer.keyboardMouse = false;
        CHECK(!InputMsg::allowed(QStringLiteral("pen"), viewer));
        CHECK(InputMsg::allowed(QStringLiteral("pen"), InputMsg::Policy{}));
    }

    // ── The pen as the host was told about it ───────────────────────────────
    {
        PenHostState host;
        auto out = host.apply(event(Pen::kHover));
        CHECK_EQ(out.size(), size_t(1));
        CHECK(!host.down());

        out = host.apply(event(Pen::kDown));
        CHECK_EQ(out.size(), size_t(1));
        CHECK_EQ(out[0].eventType, Pen::kDown);
        CHECK(host.down());

        out = host.apply(event(Pen::kMove, 0.6f));
        CHECK_EQ(out[0].eventType, Pen::kMove);

        out = host.apply(event(Pen::kUp, 0.6f));
        CHECK_EQ(out.size(), size_t(1));
        CHECK_EQ(out[0].eventType, Pen::kUp);
        CHECK(!host.down());
    }

    // Lifted by the watchdog under a stroke: down again with the next move,
    // and the page's late up is not sent twice.
    {
        PenHostState host;
        host.apply(event(Pen::kDown, 0.2f, 0.3f, Pen::kToolEraser));
        host.apply(event(Pen::kMove, 0.4f, 0.3f, Pen::kToolEraser));

        auto lift = host.lift();
        CHECK_EQ(lift.size(), size_t(2));
        CHECK_EQ(lift[0].eventType, Pen::kUp);
        CHECK(std::fabs(lift[0].x - 0.4f) < 1e-6f);
        CHECK_EQ(lift[0].toolType, Pen::kToolEraser);
        CHECK_EQ(lift[0].pressureOrDistance, 0.0f);
        CHECK_EQ(lift[1].eventType, Pen::kHoverLeave);
        CHECK(!host.down());
        CHECK(host.lift().empty());

        auto again = host.apply(event(Pen::kMove, 0.5f, 0.3f, Pen::kToolEraser));
        CHECK_EQ(again.size(), size_t(1));
        CHECK_EQ(again[0].eventType, Pen::kDown);
        CHECK(host.down());

        host.lift();
        CHECK(host.apply(event(Pen::kUp)).empty());
    }

    // A hover or a leave for a pen still down lifts it first.
    {
        PenHostState host;
        host.apply(event(Pen::kDown, 0.1f, 0.1f, Pen::kToolEraser));
        auto out = host.apply(event(Pen::kHover, 0.2f, 0.2f, Pen::kToolUnknown));
        CHECK_EQ(out.size(), size_t(2));
        CHECK_EQ(out[0].eventType, Pen::kUp);
        CHECK_EQ(out[0].toolType, Pen::kToolEraser);
        CHECK_EQ(out[1].eventType, Pen::kHover);
        CHECK(!host.down());

        host.apply(event(Pen::kDown));
        out = host.apply(event(Pen::kHoverLeave));
        CHECK_EQ(out.size(), size_t(2));
        CHECK_EQ(out[0].eventType, Pen::kUp);
        CHECK_EQ(out[1].eventType, Pen::kHoverLeave);
    }

    // A second down while down is the same stroke going on.
    {
        PenHostState host;
        host.apply(event(Pen::kDown));
        auto out = host.apply(event(Pen::kDown, 0.7f));
        CHECK_EQ(out[0].eventType, Pen::kMove);
        // Nothing for an event no page sends.
        CHECK(host.apply(event(0x04)).empty());
    }
}
