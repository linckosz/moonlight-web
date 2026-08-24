/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Working out what a host runs without asking its owner.
 *
 * The reason this file exists is a negative result that is easy to forget and
 * expensive to rediscover: the GameStream serverinfo CANNOT tell Sunshine,
 * Apollo and Wolf apart. All three impersonate GeForce Experience so Moonlight
 * clients accept them, so all three report the same appversion, the same
 * GfeVersion and the same state. The strings below are the real ones — captured
 * from two live Sunshine hosts, and read out of Wolf's own source
 * (moonlight-protocol/moonlight/protocol.hpp: M_VERSION = "7.1.431.-1",
 * M_GFE_VERSION = "3.23.0.74"; moonlight.cpp emits "SUNSHINE_SERVER_FREE").
 *
 * The Sunshine and Wolf cases here are therefore asserted to reach the SAME
 * verdict on purpose. A future change that makes them differ has either found
 * a real new marker — in which case these tests should be rewritten knowingly —
 * or, far more likely, is matching a constant three projects deliberately
 * share, and will misclassify somebody's host.
 */
#include "test_framework.h"

#include "../src/backend/streambackend/BackendProbe.h"

namespace {

// Captured live from the DualRTX Sunshine host, 2026-08-24, unpaired.
const char* const kSunshineServerInfo =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root status_code=\"200\"><hostname>DualRTX</hostname>"
    "<appversion>7.1.431.-1</appversion><GfeVersion>3.23.0.74</GfeVersion>"
    "<uniqueid>03BA228B-B3FF-982C-F417-613C24E54663</uniqueid>"
    "<HttpsPort>47984</HttpsPort><ExternalPort>47989</ExternalPort>"
    "<MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>"
    "<mac>00:00:00:00:00:00</mac><LocalIP>127.0.0.1</LocalIP>"
    "<ServerCodecModeSupport>2032385</ServerCodecModeSupport>"
    "<PairStatus>0</PairStatus><currentgame>0</currentgame>"
    "<state>SUNSHINE_SERVER_FREE</state></root>";

// The same document as Wolf builds it (field order per moonlight.cpp). Note
// that every value a fingerprint would key on is identical to Sunshine's.
const char* const kWolfServerInfo =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root status_code=\"200\"><hostname>wolf</hostname>"
    "<appversion>7.1.431.-1</appversion><GfeVersion>3.23.0.74</GfeVersion>"
    "<uniqueid>0f8fad5b-d9cb-469f-a165-70867728950e</uniqueid>"
    "<MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>"
    "<ServerCodecModeSupport>65793</ServerCodecModeSupport>"
    "<HttpsPort>47984</HttpsPort><ExternalPort>47989</ExternalPort>"
    "<mac>00:00:00:00:00:00</mac><LocalIP>10.0.0.5</LocalIP>"
    "<PairStatus>0</PairStatus><currentgame>0</currentgame>"
    "<state>SUNSHINE_SERVER_FREE</state></root>";

// The original NVIDIA software — the one server that does name itself.
const char* const kGfeServerInfo =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root status_code=\"200\"><hostname>SHIELD-PC</hostname>"
    "<appversion>7.1.431.0</appversion><GfeVersion>3.23.0.74</GfeVersion>"
    "<uniqueid>1234ABCD</uniqueid>"
    "<state>MJOLNIR_SERVER_AVAILABLE</state></root>";

} // namespace

void run_backend_probe_tests()
{
    using BackendProbe::Detected;

    SECTION("BackendProbe — serverinfo names the family, never the product");

    CHECK(BackendProbe::classifyServerInfo(QString::fromLatin1(kSunshineServerInfo)) ==
          Detected::GameStream);

    // The load-bearing assertion of this file: Wolf is NOT distinguishable here,
    // and a classifier that claims otherwise is reading a shared constant.
    CHECK(BackendProbe::classifyServerInfo(QString::fromLatin1(kWolfServerInfo)) ==
          Detected::GameStream);
    CHECK(BackendProbe::classifyServerInfo(QString::fromLatin1(kSunshineServerInfo)) ==
          BackendProbe::classifyServerInfo(QString::fromLatin1(kWolfServerInfo)));

    // MJOLNIR is the one real marker, and it matters: a GFE host cannot host any
    // of the control APIs, so there is nothing to look for on it.
    CHECK(BackendProbe::classifyServerInfo(QString::fromLatin1(kGfeServerInfo)) ==
          Detected::NvidiaGfe);

    SECTION("BackendProbe — nothing usable is not a guess");

    // Silence, a truncated body and a page from something else entirely must all
    // land on Unknown rather than the permissive branch: offering a MultiSeat
    // setup on a printer's web server would be worse than offering nothing.
    CHECK(BackendProbe::classifyServerInfo(QString()) == Detected::Unknown);
    CHECK(BackendProbe::classifyServerInfo(QStringLiteral("<root/>")) == Detected::Unknown);
    CHECK(BackendProbe::classifyServerInfo(QStringLiteral("<html><body>hi</body></html>")) ==
          Detected::Unknown);

    // A reduced serverinfo — some forks answer one before pairing — still says
    // "speaks GameStream", because uniqueid plus appversion is a shape nothing
    // else produces.
    CHECK(BackendProbe::classifyServerInfo(
              QStringLiteral("<root status_code=\"200\"><uniqueid>x</uniqueid>"
                             "<appversion>7.1.431.-1</appversion></root>")) ==
          Detected::GameStream);

    SECTION("BackendProbe — the MultiSeat control API answer");

    // The whole detection rests on this one public endpoint, so pin its shape.
    CHECK(BackendProbe::looksLikeMultiSeatAuth("{\"authEnabled\":true}"));
    // authEnabled false is still MultiSeat — it says a key will not be needed,
    // which is a different question from whether MultiSeat is there.
    CHECK(BackendProbe::looksLikeMultiSeatAuth("{\"authEnabled\":false}"));

    // Anything else on port 9550 is somebody else's service. These must not
    // register, or a host would be offered a setup that cannot complete.
    CHECK(!BackendProbe::looksLikeMultiSeatAuth("{}"));
    CHECK(!BackendProbe::looksLikeMultiSeatAuth("{\"authEnabled\":\"true\"}"));
    CHECK(!BackendProbe::looksLikeMultiSeatAuth("[{\"authEnabled\":true}]"));
    CHECK(!BackendProbe::looksLikeMultiSeatAuth("not json at all"));
    CHECK(!BackendProbe::looksLikeMultiSeatAuth(QByteArray()));

    SECTION("BackendProbe — names crossing to the browser");

    CHECK_EQ(BackendProbe::toString(Detected::GameStream), QStringLiteral("gamestream"));
    CHECK_EQ(BackendProbe::toString(Detected::NvidiaGfe), QStringLiteral("gfe"));
    CHECK_EQ(BackendProbe::toString(Detected::Unknown), QStringLiteral("unknown"));
}
