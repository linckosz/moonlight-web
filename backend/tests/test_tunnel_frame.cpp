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

    SECTION("TunnelFrame — the deflated body is a zlib stream the browser inflates");

    // The same vector as frontend/test/tunnelFrame.test.js, made by Python's
    // zlib at level 6. DecompressionStream('deflate') takes exactly this
    // (RFC 1950); qCompress puts a 4-byte length of its own in front, which
    // deflate() drops. The exact bytes of our own output are not pinned, since
    // they may change with zlib's version; that they inflate back is.
    const QByteArray plain = QByteArray::fromHex(
        "636f6e736f6c652e6c6f6728224d6f6f6e6c6967687457656222293b20636f6e736f6c652e6c6f6728"
        "224d6f6f6e6c6967687457656222293b20636f6e736f6c652e6c6f6728224d6f6f6e6c69676874576562"
        "22293b0a");
    const QByteArray vector = QByteArray::fromHex(
        "789c4bcecf2bcecf49d5cbc94fd750f2cdcfcfcbc94ccf28094f4d52d2b456482657920b005b0e1e51");
    CHECK_EQ(TunnelFrame::inflate(vector, plain.size()), plain);

    const QByteArray ours = TunnelFrame::deflate(plain);
    // A zlib header: CM 8 (deflate), and the two bytes a multiple of 31.
    CHECK(ours.size() > 2);
    CHECK_EQ(static_cast<quint8>(ours[0]) & 0x0F, 8);
    CHECK_EQ(((static_cast<quint8>(ours[0]) << 8) | static_cast<quint8>(ours[1])) % 31, 0);
    CHECK_EQ(TunnelFrame::inflate(ours, plain.size()), plain);

    // A file the size of the application's large scripts, several chunks deflated.
    QByteArray script;
    for (int i = 0; i < 6000; ++i)
        script.append("export function f" + QByteArray::number(i) + "() { return " +
                      QByteArray::number(i * 7) + "; }\n");
    const QByteArray scriptZ = TunnelFrame::deflate(script);
    CHECK(scriptZ.size() < script.size() / 3);
    CHECK_EQ(TunnelFrame::inflate(scriptZ, script.size()), script);

    SECTION("TunnelFrame — only the interface's files are deflated, never an /api/ answer");

    CHECK(TunnelFrame::mayDeflate(QStringLiteral("GET"), QStringLiteral("/js/app.js")));
    CHECK(TunnelFrame::mayDeflate(QStringLiteral("GET"), QStringLiteral("/index.html")));
    // The API carries sessions, PINs and keys beside text a caller can choose.
    CHECK(!TunnelFrame::mayDeflate(QStringLiteral("GET"), QStringLiteral("/api/app/manifest")));
    CHECK(!TunnelFrame::mayDeflate(QStringLiteral("GET"), QStringLiteral("/api/auth/status")));
    CHECK(!TunnelFrame::mayDeflate(QStringLiteral("POST"), QStringLiteral("/js/app.js")));

    const QString js = QStringLiteral("application/javascript; charset=utf-8");
    CHECK(TunnelFrame::worthDeflating(200, js, 4096));
    CHECK(TunnelFrame::worthDeflating(200, QStringLiteral("text/css; charset=utf-8"), 4096));
    CHECK(TunnelFrame::worthDeflating(200, QStringLiteral("text/html; charset=utf-8"), 4096));
    CHECK(TunnelFrame::worthDeflating(200, QStringLiteral("application/json"), 4096));
    CHECK(TunnelFrame::worthDeflating(200, QStringLiteral("image/svg+xml"), 4096));
    // Too short to be worth it, not a whole file, or already compressed.
    CHECK(!TunnelFrame::worthDeflating(200, js, TunnelFrame::kDeflateMinBytes - 1));
    CHECK(!TunnelFrame::worthDeflating(304, js, 4096));
    CHECK(!TunnelFrame::worthDeflating(206, js, 4096));
    CHECK(!TunnelFrame::worthDeflating(200, QStringLiteral("image/png"), 4096));
    CHECK(!TunnelFrame::worthDeflating(200, QStringLiteral("font/woff2"), 4096));
    // The shortcut's manifest carries the machine's key.
    CHECK(!TunnelFrame::worthDeflating(200, QStringLiteral("application/manifest+json"), 4096));
}
