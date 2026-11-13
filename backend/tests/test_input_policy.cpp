/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * What a stream's client may send to the host, and a TV on plain HTTP above
 * all (issue #42): a whitelist of messages, walked type by type, never an
 * unrestricted session, and the user's own desktop only.
 */

#include "test_framework.h"
#include "../src/streaming/InputPolicy.h"
#include "../src/streaming/MicUplink.h"
#include "platform/SignInScreen.h"

#include <QStringList>

void run_input_policy_tests()
{
    using namespace InputMsg;

    // Every type a client sends, on any of the three relays and the signalling
    // fallback, as read in the code on 13/11/2026 (DataChannelRelay,
    // MediaTrackRelay, StreamRelay, SignalingServer::handleWsFallbackInput).
    const QStringList tvMay = {
        "gamepad",       "gamepadconnect", "gamepaddisconnect", "keydown",    "keyup",
        "inputstate",    "locksync",       "textinput",         "mousemove",  "mousedown",
        "mouseup",       "mousewheel",     "mousehwheel",       "cursormode", "ping",
        "requestidr",    "request_idr",    "invalidateref",     "framefloor", "clientrefresh",
        "clientbitrate", "clientfpscap",   "clientstats",       "linkstats",  "decodequeue",
        "vsyncgrid",     "fpsstep",        "aroadnack",         "aroadack"};
    const QStringList tvMayNot = {
        "secureattention",    "unblockinput", "clipboardpaste", "hidattach",
        "hiddetach",          "hidreply",     "uprobe",         "",
        "a-type-added-later", "KEYDOWN",      "gamepadx",       "hid"};

    SECTION("InputPolicy — a TV on plain HTTP: the messages it may send, one by one");

    Policy tv;
    tv.gamepad = true;
    tv.keyboardMouse = true;
    tv.hid = false;
    tv.plainHttp = true;
    for (const QString& type : tvMay) {
        CHECK(allowedPlainHttp(type));
        CHECK(allowed(type, tv));
    }
    // Ctrl+Alt+Del, minimising every window, the clipboard, a HID device, the
    // bench's probe, and any type a later version adds: refused until named.
    for (const QString& type : tvMayNot) {
        CHECK(!allowedPlainHttp(type));
        CHECK(!allowed(type, tv));
    }

    SECTION("InputPolicy — a TV is never unrestricted");

    CHECK(!tv.unrestricted());
    // Hence no microphone (and no clipboard, Session.cpp), even on this machine.
    CHECK(!MicUplink::refusal(true, tv.unrestricted()).empty());
    // A TV without a keyboard in its policy still has none.
    Policy padOnly = tv;
    padOnly.keyboardMouse = false;
    CHECK(allowed("gamepad", padOnly));
    CHECK(!allowed("keydown", padOnly));
    CHECK(!allowed("mousemove", padOnly));
    CHECK(allowed("ping", padOnly));

    SECTION("InputPolicy — the owner and the guests, unchanged");

    const Policy owner;
    CHECK(owner.unrestricted());
    CHECK(!owner.plainHttp);
    for (const QString& type : tvMay)
        CHECK(allowed(type, owner));
    CHECK(allowed("secureattention", owner));
    CHECK(allowed("clipboardpaste", owner));
    CHECK(allowed("hidattach", owner));
    CHECK(allowed("unblockinput", owner));

    Policy guest;
    guest.gamepad = true;
    guest.keyboardMouse = false;
    guest.hid = false;
    CHECK(!guest.unrestricted());
    CHECK(allowed("gamepad", guest));
    CHECK(!allowed("keydown", guest));
    CHECK(!allowed("hidattach", guest));
    CHECK(allowed("ping", guest));
    CHECK(allowed("inputstate", guest));

    SECTION("SignInScreen — the user's own desktop");

    using mw::native::platform::isUserDesktop;
    CHECK(isUserDesktop("Default"));
    CHECK(isUserDesktop("default"));
    CHECK(isUserDesktop("DEFAULT"));
    // The secure desktop, as SYSTEM reads it and as a user's process does.
    CHECK(!isUserDesktop("Winlogon"));
    CHECK(!isUserDesktop(""));
    CHECK(!isUserDesktop("Screen-saver"));
    CHECK(!isUserDesktop("Defaul"));
    CHECK(!isUserDesktop("Default "));
    CHECK(!isUserDesktop("Defaultx"));
}
