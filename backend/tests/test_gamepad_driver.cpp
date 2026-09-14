/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Covers the guard on the ViGEmBus install notice. The threat this encodes: the
 * driver lands on the machine running the SERVER, not on the one running the
 * browser. Offering that button to a visitor — a PC on the same LAN, or anyone
 * arriving through the rendezvous — proposes a change to a machine they cannot
 * see and have no reason to suspect they are touching.
 *
 * So the rule is strictly "the socket peer is loopback", and every other
 * near-miss below (private LAN, mesh VPN, internet, tunnel) must be a refusal.
 */
#include "test_framework.h"
#include "backend/GamepadDriver.h"
#include "common/WinSystemPath.h"

namespace {

GamepadDriver::Status absent()
{
    GamepadDriver::Status st;
    st.supported = true;
    st.present = false;
    return st;
}

GamepadDriver::Status installed()
{
    GamepadDriver::Status st;
    st.supported = true;
    st.present = true;
    return st;
}

} // namespace

void run_gamepad_driver_tests()
{
    SECTION("GamepadDriver — the notice is offered to this machine only");

    using GamepadDriver::mayOffer;
    using NetClassify::Kind;

    // The one case that says yes: sitting at the host, driver missing.
    CHECK(mayOffer(Kind::Loopback, false, absent()));

    // Another PC on the same LAN. It classifies as Private, and isPrivateOrLoopback()
    // would have let it through — which is exactly the mistake this guard exists
    // to avoid.
    CHECK(!mayOffer(Kind::Private, false, absent()));

    // A mesh VPN peer (Tailscale and friends) and a genuine internet visitor.
    // Both reach us over the rendezvous in practice; neither is this machine.
    CHECK(!mayOffer(Kind::Tunnel, false, absent()));
    CHECK(!mayOffer(Kind::Public, false, absent()));

    // The rendezvous control tunnel. Its requests never touched a socket here,
    // so the peer address is not the browser's — refused whatever it says, and
    // in particular when it says loopback.
    CHECK(!mayOffer(Kind::Loopback, true, absent()));
    CHECK(!mayOffer(Kind::Private, true, absent()));

    SECTION("GamepadDriver — nothing to offer when there is nothing to install");

    // Driver already there: no notice, from anywhere.
    CHECK(!mayOffer(Kind::Loopback, false, installed()));

    // A platform with no virtual-pad backend at all. Installing anything would
    // change nothing, so the notice must not appear even at the machine itself.
    GamepadDriver::Status unsupported;
    unsupported.supported = false;
    unsupported.present = false;
    CHECK(!mayOffer(Kind::Loopback, false, unsupported));

    SECTION("GamepadDriver — the address the notice links to");

    // Stated once in the code so the notice and the installer cannot drift onto
    // two different driver versions.
    CHECK(GamepadDriver::downloadUrl().startsWith(
        QStringLiteral("https://github.com/nefarius/ViGEmBus/releases/")));

    SECTION("Staging — never under System32, where a 32-bit installer cannot find itself");

    // Pure string logic, so it runs on every platform; SystemRoot is pinned for
    // the duration so a Windows runner's real value cannot change the answer.
    const QByteArray savedRoot = qgetenv("SystemRoot");
    qputenv("SystemRoot", "C:\\WINDOWS");

    using mw::win::isUnderSystem32;
    // What a LocalSystem service reports as LOCALAPPDATA — the path every
    // service update was staged under while it silently failed. Case differs
    // from SystemRoot on purpose: Windows hands out both spellings.
    CHECK(isUnderSystem32(
        QStringLiteral("C:\\Windows\\system32\\config\\systemprofile\\AppData\\Local")));
    CHECK(isUnderSystem32(QStringLiteral("C:/WINDOWS/System32")));
    // The fallbacks and an ordinary user profile are fine.
    CHECK(!isUnderSystem32(QStringLiteral("C:\\Users\\alice\\AppData\\Local")));
    CHECK(!isUnderSystem32(QStringLiteral("C:/Program Files/MoonlightWeb")));
    // A sibling that merely starts with the same letters is not inside it.
    CHECK(!isUnderSystem32(QStringLiteral("C:\\Windows\\System32Backup\\x")));
    CHECK(!isUnderSystem32(QStringLiteral("C:\\Windows\\SysWOW64\\config\\systemprofile")));

    if (savedRoot.isNull())
        qunsetenv("SystemRoot");
    else
        qputenv("SystemRoot", savedRoot);
}
