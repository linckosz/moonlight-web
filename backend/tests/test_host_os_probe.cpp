/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Which OS a host runs, and the one behaviour that hangs on the answer.
 *
 * The behaviour is scroll quantization. A Linux host throws away any scroll
 * amount below a whole 120-unit notch (inputtino: REL_WHEEL = distance / 120,
 * no accumulator), so the client has to hold sub-notch amounts back or a
 * trackpad scrolls by nothing at all. Windows and macOS both keep the
 * leftovers, and holding amounts back for them is a real regression: a
 * trackpad or a high-res wheel turns into a ratchet.
 *
 * So the asymmetry asserted below is deliberate and load-bearing. A verdict of
 * Unknown must behave like Linux, never like Windows: a host that scrolls
 * chunkily still scrolls, and one that drops every sub-notch delta does not.
 * Anything here that starts letting Unknown through has traded a broken
 * feature for a smooth one on hosts it cannot identify.
 */
#include "test_framework.h"

#include "../src/backend/streambackend/HostOsProbe.h"

void run_host_os_probe_tests()
{
    using HostOsProbe::HostOs;
    using HostOsProbe::OsEvidence;

    SECTION("HostOsProbe — which hosts keep a sub-notch scroll amount");

    // Windows accumulates in SendInput; libvirtualhid's Windows backend keeps a
    // vertical_scroll_remainder_ besides. macOS converts to CoreGraphics pixels
    // (scroll_pixels multiplies by pixels-per-line and lines-per-detent BEFORE
    // dividing by 120), so a sub-notch amount still moves the page.
    CHECK(HostOsProbe::keepsSubNotchScroll(HostOs::Windows));
    CHECK(HostOsProbe::keepsSubNotchScroll(HostOs::MacOs));

    // Linux does not. This is the whole reason the module exists.
    CHECK(!HostOsProbe::keepsSubNotchScroll(HostOs::Linux));

    // And an unidentified host is treated as the one that breaks, not the one
    // that is merely less smooth.
    CHECK(!HostOsProbe::keepsSubNotchScroll(HostOs::Unknown));

    SECTION("HostOsProbe — a host that is this very machine needs no inference");

    OsEvidence self;
    self.isLocalMachine = true;
    CHECK(HostOsProbe::infer(self) == HostOsProbe::thisMachine());

    // It also outranks everything else: an address of ours is not a guess, and
    // no probe result can be more certain than that.
    OsEvidence selfWithNoise = self;
    selfWithNoise.backendType = QStringLiteral("wolf");
    selfWithNoise.observedIpTtl = 64;
    CHECK(HostOsProbe::infer(selfWithNoise) == HostOsProbe::thisMachine());

    SECTION("HostOsProbe — a backend that identified itself carries its platform");

    OsEvidence wolf;
    wolf.backendType = QStringLiteral("wolf");
    CHECK(HostOsProbe::infer(wolf) == HostOs::Linux);

    OsEvidence multiseat;
    multiseat.backendType = QStringLiteral("multiseat");
    CHECK(HostOsProbe::infer(multiseat) == HostOs::Windows);

    // The API answering is enough on its own — a host can be running MultiSeat
    // without anyone having pointed MoonlightWeb at it yet.
    OsEvidence multiseatSeen;
    multiseatSeen.multiSeatApiPresent = true;
    CHECK(HostOsProbe::infer(multiseatSeen) == HostOs::Windows);

    // GeForce Experience never shipped off Windows.
    OsEvidence gfe;
    gfe.isNvidiaServerSoftware = true;
    CHECK(HostOsProbe::infer(gfe) == HostOs::Windows);

    // A backend name we do not know is not evidence of anything.
    OsEvidence other;
    other.backendType = QStringLiteral("gamestream");
    CHECK(HostOsProbe::infer(other) == HostOs::Unknown);

    SECTION("HostOsProbe — the TTL fingerprint, and where it stops");

    // Windows stacks start at 128, and every router on the way only lowers it,
    // so nothing that started at 64 can arrive above it.
    CHECK(HostOsProbe::fromInitialTtl(128) == HostOs::Windows);
    CHECK(HostOsProbe::fromInitialTtl(127) == HostOs::Windows); // one hop
    CHECK(HostOsProbe::fromInitialTtl(115) == HostOs::Windows); // a long WAN path
    CHECK(HostOsProbe::fromInitialTtl(65) == HostOs::Windows);  // 63 hops, still Windows

    // 64 and below began at 64 — Linux OR macOS. Those two want OPPOSITE
    // handling, so this is exactly where the fingerprint has to stop. Claiming
    // Linux here would quantize every macOS host for no reason; claiming macOS
    // would break every Linux one. Unknown is the only honest answer, and it
    // happens to be the safe one.
    CHECK(HostOsProbe::fromInitialTtl(64) == HostOs::Unknown);
    CHECK(HostOsProbe::fromInitialTtl(56) == HostOs::Unknown);
    CHECK(HostOsProbe::fromInitialTtl(1) == HostOs::Unknown);

    // Never observed at all, and 255-based stacks (nothing that speaks
    // GameStream), both say nothing.
    CHECK(HostOsProbe::fromInitialTtl(0) == HostOs::Unknown);
    CHECK(HostOsProbe::fromInitialTtl(255) == HostOs::Unknown);
    CHECK(HostOsProbe::fromInitialTtl(-1) == HostOs::Unknown);

    // Live capture, 2026-08-30: the Ubuntu host at 192.168.1.50 replies TTL 64
    // on the LAN. It reads as Unknown, and quantizes — which is right for it.
    OsEvidence unixLike;
    unixLike.observedIpTtl = 64;
    CHECK(HostOsProbe::infer(unixLike) == HostOs::Unknown);
    CHECK(!HostOsProbe::keepsSubNotchScroll(HostOsProbe::infer(unixLike)));

    // A remote Sunshine host on Windows: nothing on the control plane names it
    // (its REST API answers 401 to everything and /api/configLocale carries
    // only a locale), so the TTL its media packets arrive with is the ONLY
    // thing that gets scrolling smooth there. This is that path.
    OsEvidence remoteWindows;
    remoteWindows.observedIpTtl = 126;
    CHECK(HostOsProbe::infer(remoteWindows) == HostOs::Windows);
    CHECK(HostOsProbe::keepsSubNotchScroll(HostOsProbe::infer(remoteWindows)));

    // And it stays the LAST word: a proven backend outranks a fingerprint.
    // A Wolf container behind a NAT could plausibly show a high TTL; the type
    // it identified itself as still wins.
    OsEvidence wolfBehindNat;
    wolfBehindNat.backendType = QStringLiteral("wolf");
    wolfBehindNat.observedIpTtl = 120;
    CHECK(HostOsProbe::infer(wolfBehindNat) == HostOs::Linux);

    SECTION("HostOsProbe — the names that cross the process boundary");

    // A stream worker gets the verdict as a string on stdin, so the round trip
    // has to survive exactly. A typo here silently re-quantizes every host.
    for (HostOs os : {HostOs::Unknown, HostOs::Windows, HostOs::Linux, HostOs::MacOs})
        CHECK(HostOsProbe::fromString(HostOsProbe::toString(os)) == os);

    // An older parent sends no field at all, and an unreadable one must not
    // become a claim about the host.
    CHECK(HostOsProbe::fromString(QString()) == HostOs::Unknown);
    CHECK(HostOsProbe::fromString(QStringLiteral("Windows")) == HostOs::Unknown);
}
