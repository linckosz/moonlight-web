/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "network/MdnsAdvertiser.h"

namespace {

using Txt = QList<QPair<QString, QString>>;

QPair<QString, QString> entry(const char* key, const QString& value)
{
    return qMakePair(QString::fromLatin1(key), value);
}

/// The `name` a TXT record carries, empty when it carries none.
QString txtName(const QString& name)
{
    const Txt txt = MdnsAdvertiser::txtRecord(QStringLiteral("hostid"), name);
    for (const QPair<QString, QString>& e : txt)
        if (e.first == QLatin1String("name")) return e.second;
    return {};
}

// The TXT record beside the name: which machine it is (`id`) and what it is
// called (`name`), the name read by the TV app as UTF-8, one string of at most
// 63 bytes.
void testTxtRecord()
{
    SECTION("MdnsAdvertiser: the TXT record");

    const QString id = QStringLiteral("4f2a9c0e1b7d3a56");
    // In the order announced: the version, the host key's name, the name.
    const Txt named{entry("v", "1"), entry("id", id), entry("name", "Living-room PC")};
    CHECK(MdnsAdvertiser::txtRecord(id, QStringLiteral("Living-room PC")) == named);
    // Nothing left of the name: no `name` at all, the rest as before.
    const Txt bare{entry("v", "1"), entry("id", id)};
    CHECK(MdnsAdvertiser::txtRecord(id, QString()) == bare);
    CHECK(MdnsAdvertiser::txtRecord(id, QStringLiteral("   ")) == bare);
    CHECK_EQ(txtName(QStringLiteral("  DualRTX ")), QStringLiteral("DualRTX"));

    // 63 bytes kept whole; one more is cut.
    const QString ascii63(63, QLatin1Char('x'));
    CHECK_EQ(txtName(ascii63), ascii63);
    CHECK_EQ(txtName(ascii63 + QLatin1Char('y')), ascii63);

    // Cut between two characters, never inside one. 32 times é is 64 bytes:
    // the last one would straddle the limit, and goes whole.
    CHECK_EQ(txtName(QString(32, QChar(0x00E9))), QString(31, QChar(0x00E9)));
    const QString x62(62, QLatin1Char('x'));
    CHECK_EQ(txtName(x62 + QChar(0x00E9)), x62);
    // Three bytes a character: 21 of 32 are exactly 63 bytes.
    const QString cut = txtName(QString(32, QChar(0x7535)));
    CHECK_EQ(cut, QString(21, QChar(0x7535)));
    CHECK_EQ(cut.toUtf8().size(), 63);
    // Four bytes, and two UTF-16 units, a character: never half of one.
    QString pads;
    for (int i = 0; i < 16; ++i)
        pads += QString::fromUcs4(U"\U0001F3AE", 1);
    const QString pad15 = txtName(pads);
    CHECK_EQ(pad15, pads.left(30));
    CHECK_EQ(pad15.toUtf8().size(), 60);
    // And no space left hanging where the cut fell.
    const QString x61(61, QLatin1Char('x'));
    CHECK_EQ(txtName(x61 + QLatin1Char(' ') + QString(2, QChar(0x00E9))), x61);
}

// The same record as Avahi takes it: one `key=value` string each, in UTF-8,
// in the order announced, the name already cut.
void testTxtStrings()
{
    SECTION("MdnsAdvertiser: the TXT strings");

    const QString id = QStringLiteral("4f2a9c0e1b7d3a56");
    const QList<QByteArray> named{"v=1", "id=4f2a9c0e1b7d3a56", "name=Salon \xC3\xA9t\xC3\xA9"};
    CHECK(MdnsAdvertiser::txtStrings(id, QStringLiteral("  Salon %1t%1 ").arg(QChar(0x00E9))) ==
          named);
    const QList<QByteArray> bare{"v=1", "id=4f2a9c0e1b7d3a56"};
    CHECK(MdnsAdvertiser::txtStrings(id, QString()) == bare);
    // A name of 64 bytes goes out as 63, plus its key: one string under 255.
    const QList<QByteArray> cut = MdnsAdvertiser::txtStrings(id, QString(64, QLatin1Char('x')));
    CHECK_EQ(cut.size(), 3);
    CHECK(cut.value(2) == QByteArray("name=") + QByteArray(63, 'x'));
}

} // namespace

// The name the TV app lists this server under. A dot would make the instance
// two labels, a name over 63 bytes is refused by the responder, and a --dev
// instance on the same PC must not collide with the production one.
void run_mdns_advertiser_tests()
{
    SECTION("MdnsAdvertiser");

    CHECK_EQ(MdnsAdvertiser::instanceName(QStringLiteral("DualRTX"), false),
             QStringLiteral("DualRTX"));
    CHECK_EQ(MdnsAdvertiser::instanceName(QStringLiteral("DualRTX"), true),
             QStringLiteral("DualRTX DEV"));
    CHECK_EQ(MdnsAdvertiser::instanceName(QStringLiteral("pc.home.lan"), false),
             QStringLiteral("pc"));
    CHECK_EQ(MdnsAdvertiser::instanceName(QStringLiteral("  "), false),
             QStringLiteral("MoonlightWeb"));

    const QString longName(80, QLatin1Char('x'));
    CHECK_EQ(MdnsAdvertiser::instanceName(longName, false).toUtf8().size(), 63);
    const QString dev = MdnsAdvertiser::instanceName(longName, true);
    CHECK(dev.endsWith(QStringLiteral(" DEV")));
    CHECK_EQ(dev.toUtf8().size(), 63);

    // UTF-8 is cut on a character, never inside one.
    const QString accents(40, QChar(0x00E9)); // 80 bytes
    const QString cut = MdnsAdvertiser::instanceName(accents, false);
    CHECK_EQ(cut.size(), 31);
    CHECK_EQ(cut.toUtf8().size(), 62);

    testTxtRecord();
    testTxtStrings();
}
