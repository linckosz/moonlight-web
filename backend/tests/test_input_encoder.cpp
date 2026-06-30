/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "streaming/InputEncoder.h"

#include <QJsonObject>
#include <QtEndian>

static quint32 beU32(const QByteArray& b, int off) { return qFromBigEndian<quint32>(b.constData() + off); }
static quint32 leU32(const QByteArray& b, int off) { return qFromLittleEndian<quint32>(b.constData() + off); }

void run_input_encoder_tests()
{
    SECTION("InputEncoder");

    // Unknown event type → empty packet.
    CHECK(InputEncoder::encodeFromJson(QJsonObject{{"type", "nope"}}).isEmpty());
    CHECK(InputEncoder::encodeFromJson(QJsonObject{}).isEmpty());

    // keydown: 8-byte header + 6-byte payload, header size = 4+6, magic LE = 0x03.
    {
        QJsonObject m{{"type", "keydown"}, {"keyCode", 65}, {"ctrlKey", true}, {"shiftKey", true}};
        QByteArray p = InputEncoder::encodeFromJson(m);
        CHECK_EQ(p.size(), 14);
        CHECK_EQ(beU32(p, 0), 10u);   // header size excludes the size field
        CHECK_EQ(leU32(p, 4), 0x03u); // keydown magic
        // keyCode is stored as (vk | 0x8000) little-endian at payload[1..2] (packet[9..10]).
        quint16 vk = qFromLittleEndian<quint16>(p.constData() + 9);
        CHECK_EQ(vk, static_cast<quint16>(65 | 0x8000));
        // modifiers byte at payload[3] (packet[11]): ctrl(0x02)|shift(0x01).
        CHECK_EQ(static_cast<int>(static_cast<unsigned char>(p[11])), 0x03);
    }

    // keyup magic = 0x04.
    {
        QByteArray p = InputEncoder::encodeFromJson(QJsonObject{{"type", "keyup"}, {"keyCode", 1}});
        CHECK_EQ(leU32(p, 4), 0x04u);
    }

    // mousemove: 4-byte payload, magic 0x07.
    {
        QByteArray p = InputEncoder::encodeFromJson(
            QJsonObject{{"type", "mousemove"}, {"dx", 5}, {"dy", -3}});
        CHECK_EQ(p.size(), 12);
        CHECK_EQ(beU32(p, 0), 8u);
        CHECK_EQ(leU32(p, 4), 0x07u);
        CHECK_EQ(qFromLittleEndian<qint16>(p.constData() + 8), static_cast<qint16>(5));
        CHECK_EQ(qFromLittleEndian<qint16>(p.constData() + 10), static_cast<qint16>(-3));
    }

    // mousedown / mouseup: 1-byte payload, magic 0x08 / 0x09.
    {
        QByteArray d = InputEncoder::encodeFromJson(
            QJsonObject{{"type", "mousedown"}, {"button", 1}});
        CHECK_EQ(d.size(), 9);
        CHECK_EQ(leU32(d, 4), 0x08u);
        CHECK_EQ(static_cast<int>(static_cast<unsigned char>(d[8])), 1);
        QByteArray u = InputEncoder::encodeFromJson(QJsonObject{{"type", "mouseup"}, {"button", 2}});
        CHECK_EQ(leU32(u, 4), 0x09u);
    }

    // mousewheel: 6-byte payload, magic 0x0A, scroll mirrored big-endian.
    {
        QByteArray p = InputEncoder::encodeFromJson(
            QJsonObject{{"type", "mousewheel"}, {"delta", 120}});
        CHECK_EQ(p.size(), 14);
        CHECK_EQ(leU32(p, 4), 0x0Au);
        CHECK_EQ(qFromBigEndian<qint16>(p.constData() + 8), static_cast<qint16>(120));
        CHECK_EQ(qFromBigEndian<qint16>(p.constData() + 10), static_cast<qint16>(120));
    }
}
