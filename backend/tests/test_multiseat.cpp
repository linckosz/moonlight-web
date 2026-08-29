/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * MultiSeat: reading a seat, and deciding whether it can be streamed.
 *
 * Both are pinned because both have already been wrong once, and neither fails
 * loudly:
 *
 *  - MultiSeat answers camelCase JSON with *string* enums (set explicitly in
 *    ApiServer.cs), and its lists are bare arrays rather than the {success,…}
 *    envelope Wolf uses. A field read under the wrong name comes back as an
 *    empty string, not as an error, and a seat then looks nameless instead of
 *    unparsed.
 *  - A seat that failed to provision still carries a portBase. Deciding
 *    "streamable" on the port alone would offer a dead Apollo as if it were
 *    ready — which is exactly what listSeats() must never do.
 *
 * The port arithmetic comes from Apollo's own map_port(N) = base + N
 * (Constants.cs): the GFE HTTP port is the base, HTTPS sits five below it.
 */
#include "test_framework.h"

#include "../src/backend/MultiSeatApiClient.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

/// One row of GET /api/seats, as MultiSeat.Service actually serialises it.
const char* const kReadySeat = R"({
    "id": "3f2b1c7a-9d64-4f1e-8a55-0c9d2e6b4471",
    "accountName": "MultiSeatSeat01",
    "sessionId": 3,
    "status": "Ready",
    "width": 1920,
    "height": 1080,
    "fps": 60,
    "portBase": 48100,
    "apolloProcessId": 8244,
    "errorMessage": "",
    "provisioningStep": "Apollo"
})";

/// A seat that died mid-provision. It keeps a portBase, and its errorMessage is
/// the most actionable thing MultiSeat ever says.
const char* const kFailedSeat = R"({
    "id": "b81e0f42-1a3d-4c88-9f20-77ac5d1e3390",
    "accountName": "MultiSeatSeat02",
    "sessionId": -1,
    "status": "Error",
    "portBase": 48130,
    "errorMessage": "Audio device not found — re-run scripts/install-vbcable.ps1",
    "provisioningStep": "Audio"
})";

QJsonObject obj(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

} // namespace

void run_multiseat_tests()
{
    SECTION("MultiSeat seat parsing");

    const MultiSeatSeat ready = MultiSeatApiClient::parseSeat(obj(kReadySeat));
    CHECK(ready.id == QStringLiteral("3f2b1c7a-9d64-4f1e-8a55-0c9d2e6b4471"));
    // The Windows account is the only part of a seat an admin recognises.
    CHECK(ready.accountName == QStringLiteral("MultiSeatSeat01"));
    CHECK_EQ(ready.sessionId, 3);
    CHECK(ready.status == QStringLiteral("Ready"));
    CHECK_EQ(ready.width, 1920);
    CHECK_EQ(ready.height, 1080);
    CHECK_EQ(ready.fps, 60);
    CHECK_EQ(ready.portBase, 48100);
    CHECK_EQ(ready.apolloProcessId, 8244);

    // A failed seat must keep the two fields that explain it. Reducing it to
    // "unusable" throws away the sentence that says which script to re-run.
    const MultiSeatSeat failed = MultiSeatApiClient::parseSeat(obj(kFailedSeat));
    CHECK(failed.status == QStringLiteral("Error"));
    CHECK(failed.provisioningStep == QStringLiteral("Audio"));
    CHECK(failed.errorMessage.contains(QStringLiteral("install-vbcable")));

    // Absent fields, not an error: a seat still being created answers with very
    // little, and sessionId means "not logged in" rather than session zero.
    const MultiSeatSeat sparse =
        MultiSeatApiClient::parseSeat(obj(R"({"id":"x","status":"Provisioning"})"));
    CHECK(sparse.id == QStringLiteral("x"));
    CHECK_EQ(sparse.sessionId, -1);
    CHECK_EQ(sparse.portBase, 0);
    CHECK(sparse.accountName.isEmpty());

    SECTION("MultiSeat seat ports");

    // Apollo's map_port: HTTP is the base, HTTPS is five below.
    CHECK_EQ(ready.gfeHttpPort(), quint16(48100));
    CHECK_EQ(ready.gfeHttpsPort(), quint16(48095));
    CHECK_EQ(failed.gfeHttpPort(), quint16(48130));
    CHECK_EQ(failed.gfeHttpsPort(), quint16(48125));

    // No block yet: both answer 0 rather than wrapping around to 65531, which
    // is what a bare `portBase - 5` would produce on an unsigned port.
    MultiSeatSeat unplaced;
    unplaced.portBase = 0;
    CHECK_EQ(unplaced.gfeHttpPort(), quint16(0));
    CHECK_EQ(unplaced.gfeHttpsPort(), quint16(0));

    SECTION("MultiSeat streamable seats");

    // Only a seat with a live Apollo behind it can be streamed.
    CHECK_EQ(ready.isUsable(), true);

    MultiSeatSeat streaming = ready;
    streaming.status = QStringLiteral("Streaming");
    CHECK_EQ(streaming.isUsable(), true);

    // The regression this file exists for: a failed seat has a port, and must
    // still be refused.
    CHECK_EQ(failed.isUsable(), false);

    for (const char* midway : {"Idle", "Provisioning", "Configuring", "TearingDown"}) {
        MultiSeatSeat seat = ready;
        seat.status = QString::fromLatin1(midway);
        CHECK_EQ(seat.isUsable(), false);
    }

    // Ready but portless: nothing to dial, so not streamable either.
    MultiSeatSeat portless = ready;
    portless.portBase = 0;
    CHECK_EQ(portless.isUsable(), false);
}
