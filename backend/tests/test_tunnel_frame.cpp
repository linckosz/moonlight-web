/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The bytes that cross the rendezvous control channel.
 *
 * This file exists because the other half of this format is JavaScript running
 * in someone else's browser (bootstrap/tunnel.js). Nothing catches a
 * disagreement between the two at compile time, at deploy time, or in a code
 * review — the first sign would be a user whose application will not load, with
 * no error that names the cause.
 *
 * So both sides are pinned against the SAME vectors: the hex strings below are
 * asserted here, and frontend/test/tunnelFrame.test.js asserts that the browser
 * encoder produces exactly them. A change to one side without the other fails
 * one of the two suites.
 */
#include "test_framework.h"

#include "../src/server/TunnelFrame.h"

#include <QJsonObject>

void run_tunnel_frame_tests()
{
    SECTION("TunnelFrame — the frame header is five bytes, big-endian");

    // kind 0x01, id 1, no payload. The identifier is big-endian: a browser
    // reading it the other way round would answer request 16777216 and the two
    // sides would talk past each other for every request in flight.
    CHECK_EQ(TunnelFrame::build(TunnelFrame::Request, 1, QByteArray()).toHex(),
             QByteArray("0100000001"));

    CHECK_EQ(TunnelFrame::build(TunnelFrame::End, 0x01020304, QByteArray()).toHex(),
             QByteArray("0401020304"));

    CHECK_EQ(TunnelFrame::build(TunnelFrame::WsText, 7, QByteArray("hi")).toHex(),
             QByteArray("06000000076869"));

    SECTION("TunnelFrame — a frame that cannot be read is dropped, not guessed");

    quint8 kind = 0;
    quint32 id = 0;
    QByteArray payload;

    // Four bytes is one short of a header. Reading an id out of it would read
    // past the end of the message.
    CHECK(!TunnelFrame::parse(QByteArray::fromHex("01000000"), &kind, &id, &payload));
    CHECK(!TunnelFrame::parse(QByteArray(), &kind, &id, &payload));

    CHECK(TunnelFrame::parse(QByteArray::fromHex("06000000076869"), &kind, &id, &payload));
    CHECK_EQ(static_cast<int>(kind), static_cast<int>(TunnelFrame::WsText));
    CHECK_EQ(static_cast<int>(id), 7);
    CHECK_EQ(payload, QByteArray("hi"));

    // An empty payload is legitimate and must not be confused with a failure:
    // END and WS_OPENED carry nothing at all.
    CHECK(TunnelFrame::parse(QByteArray::fromHex("0400000001"), &kind, &id, &payload));
    CHECK(payload.isEmpty());

    SECTION("TunnelFrame — the head is length-prefixed so the body starts where it says");

    const QByteArray encoded = TunnelFrame::encodeHead(
        QJsonObject{{QStringLiteral("m"), QStringLiteral("GET")}}, QByteArray("BODY"));
    // 4-byte length, then {"m":"GET"} (11 bytes), then the body verbatim.
    CHECK_EQ(encoded.toHex(), QByteArray("0000000b7b226d223a22474554227d424f4459"));

    QJsonObject head;
    QByteArray body;
    CHECK(TunnelFrame::decodeHead(encoded, &head, &body));
    CHECK_EQ(head.value(QStringLiteral("m")).toString(), QStringLiteral("GET"));
    CHECK_EQ(body, QByteArray("BODY"));

    SECTION("TunnelFrame — a head length that runs past the end is refused");

    // The shape a truncated frame takes, and the shape a hostile one would take
    // deliberately: a length that would have us read memory we were not given.
    // It must be refused rather than clamped, because a clamped read produces a
    // half-parsed request that looks valid.
    QByteArray overrun;
    TunnelFrame::appendU32(overrun, 9999);
    overrun.append("{}");
    CHECK(!TunnelFrame::decodeHead(overrun, &head, &body));

    // A zero-length head is not a head. Accepting it would route a request with
    // no method and no path.
    QByteArray empty;
    TunnelFrame::appendU32(empty, 0);
    CHECK(!TunnelFrame::decodeHead(empty, &head, &body));

    // Fewer than four bytes cannot even carry the length.
    CHECK(!TunnelFrame::decodeHead(QByteArray::fromHex("000000"), &head, &body));

    // Well-formed length, contents that are not an object.
    QByteArray notJson;
    TunnelFrame::appendU32(notJson, 3);
    notJson.append("[1]");
    CHECK(!TunnelFrame::decodeHead(notJson, &head, &body));

    SECTION("TunnelFrame — the chunk size is a floor everyone implements");

    // 16 KiB is chosen so no negotiated maximum message size has to be trusted:
    // implementations differ on what they will accept, and they all accept this.
    CHECK_EQ(TunnelFrame::kChunkBytes, 16 * 1024);
    CHECK(TunnelFrame::kChunkBytes < 64 * 1024);

    // A request body has a ceiling because the API takes JSON documents, not
    // uploads. Without one, a browser could make the host hold an arbitrary
    // amount of memory before a single check has run.
    CHECK_EQ(TunnelFrame::kMaxRequestBytes, 256 * 1024);
}
