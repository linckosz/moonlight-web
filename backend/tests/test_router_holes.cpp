/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The record of what this host wrote on the router (network/RouterHoles):
 * which lines it may hold, how it reads from disk, and whose it is. A record
 * that is another installation's, another process's, or does not read as
 * one, leads to nothing.
 */
#include "test_framework.h"
#include "network/RouterHoles.h"
#include "network/RouterPortPools.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <functional>

namespace {

using namespace mw::routerholes;

const QString kLan = QStringLiteral("192.168.1.20");
const QByteArray kMachine = QByteArrayLiteral("machine-one");

Hole tunnel(uint16_t port, bool isTcp = false)
{
    Hole line;
    line.external = port;
    line.internal = port;
    line.tcp = isTcp;
    line.kind = Kind::Tunnel;
    line.lan = kLan;
    return line;
}

Hole streams(uint16_t port)
{
    Hole line = tunnel(port);
    line.kind = Kind::Streams;
    return line;
}

Hole media(int slot, uint16_t external, bool isTcp = false)
{
    Hole line;
    line.external = external;
    line.internal = static_cast<uint16_t>(mw::routerports::kMediaBasePort + slot);
    line.tcp = isTcp;
    line.kind = Kind::Media;
    line.slot = slot;
    line.lan = kLan;
    return line;
}

/// A record as serialize() writes it, to be spoiled one key at a time.
QJsonObject wholeRecord()
{
    Content content;
    content.owner = fingerprint(kMachine, QStringLiteral("/somewhere"));
    content.tag = QStringLiteral("a3f9c1");
    content.holes = {tunnel(3478), media(1, 46100, true)};
    return QJsonDocument::fromJson(serialize(content)).object();
}

QByteArray bytesOf(const QJsonObject& root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/// The record with its first line changed by `spoil`: what is left of the lines.
QList<Hole> linesAfter(const std::function<void(QJsonObject&)>& spoil)
{
    QJsonObject root = wholeRecord();
    QJsonArray holes = root.value(QStringLiteral("holes")).toArray();
    QJsonObject first = holes.at(0).toObject();
    spoil(first);
    holes[0] = first;
    root[QStringLiteral("holes")] = holes;
    Content out;
    CHECK(parse(bytesOf(root), false, out));
    return out.holes;
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

void run_router_holes_tests()
{
    SECTION("RouterHoles: the words on the router's page, and the tag");
    CHECK(describe(Kind::Tunnel, -1, {}) == QStringLiteral("MoonlightWeb tunnel"));
    CHECK(describe(Kind::Streams, -1, {}) == QStringLiteral("MoonlightWeb streams"));
    CHECK(describe(Kind::Media, 3, {}) == QStringLiteral("MoonlightWeb media slot 3"));
    CHECK(describe(Kind::Tunnel, -1, QStringLiteral("a3f9c1")) ==
          QStringLiteral("MoonlightWeb tunnel a3f9c1"));
    // The longest of them, which a router has to give back whole
    CHECK_EQ(describe(Kind::Media, 23, QStringLiteral("a3f9c1")).size(), 33);
    CHECK(validTag(QStringLiteral("a3f9c1")) && validTag(QStringLiteral("000000")));
    for (const char* bad : {"", "a3f9c", "a3f9c1d", "A3F9C1", "a3f9g1", "a3 9c1"})
        CHECK(!validTag(QString::fromLatin1(bad)));

    SECTION("RouterHoles: an address as a router writes it");
    for (const char* good : {"192.168.1.20", "10.0.0.1", "0.0.0.0", "255.255.255.255"})
        CHECK(dottedIpv4(QString::fromLatin1(good)));
    for (const char* bad : {"", "192.168.1", "192.168.1.20.1", "192.168.1.256", "192.168.1.020",
                            "192.168.1.20 ", " 192.168.1.20", "192.168..20", "192.168.1.-2",
                            "192.168.1.2a", "::1", "host", "1.2.3.4/24", "+1.2.3.4"})
        CHECK(!dottedIpv4(QString::fromLatin1(bad)));

    SECTION("RouterHoles: a line is one this kind of instance may have written");
    {
        using namespace mw::routerports;
        // The tunnel: its own lists, forwarded to itself
        CHECK(allowed(false, tunnel(3478)) && allowed(false, tunnel(5352, true)));
        CHECK(allowed(false, tunnel(kTunnelPoolBegin)) && allowed(false, tunnel(kTunnelPoolEnd)));
        CHECK(!allowed(false, tunnel(kDevTunnelPoolBegin)));
        CHECK(allowed(true, tunnel(kDevTunnelPoolBegin)) &&
              allowed(true, tunnel(kDevTunnelPoolEnd)));
        CHECK(!allowed(true, tunnel(3478)) && !allowed(true, tunnel(kTunnelPoolBegin)));
        Hole elsewhere = tunnel(3478);
        elsewhere.internal = 22;
        CHECK(!allowed(false, elsewhere));
        // Nobody's numbers
        for (const int port : {22, 80, 443, 3389, 3477, 3482, 46032, int(kKnownQuestionPort), 46200,
                               47984, 48010, 48574, 65535}) {
            CHECK(!allowed(false, tunnel(uint16_t(port))));
            CHECK(!allowed(true, tunnel(uint16_t(port))));
            CHECK(!allowed(false, streams(uint16_t(port))));
            CHECK(!allowed(true, streams(uint16_t(port))));
        }
        // The shared ports: the conferencing list and the foot of the block
        // for the installed host, the top of the block for a DEV identity
        CHECK(allowed(false, streams(3478)) && allowed(false, streams(kMediaBasePort)));
        CHECK(!allowed(false, streams(uint16_t(kMediaBasePort + kMuxBlockSpan))));
        CHECK(!allowed(false, streams(kTunnelPoolBegin)));
        const uint16_t top = uint16_t(kMediaBasePort + kMediaPortCount - 1);
        CHECK(allowed(true, streams(top)) && !allowed(true, streams(kMediaBasePort)));
        CHECK(!allowed(true, streams(3478)));
        // A slot's hole: to a port of the block, from the block or the pool
        CHECK(allowed(false, media(0, kMediaBasePort)) && allowed(true, media(23, kMediaPoolEnd)));
        CHECK(allowed(false, media(1, kMediaPoolBegin, true)));
        CHECK(!allowed(false, media(1, 3478)) && !allowed(false, media(1, 48010)));
        CHECK(!allowed(false, media(-1, kMediaPoolBegin)));
        CHECK(!allowed(false, media(kMediaPortCount, kMediaPoolBegin)));
        Hole outside = media(1, kMediaPoolBegin);
        outside.internal = 22;
        CHECK(!allowed(false, outside));
        // A tunnel or a shared port has no slot
        Hole slotted = tunnel(3478);
        slotted.slot = 0;
        CHECK(!allowed(false, slotted));
        Hole nothing;
        CHECK(!allowed(false, nothing));
    }

    SECTION("RouterHoles: the file, read strictly");
    {
        Content content;
        content.owner = fingerprint(kMachine, QStringLiteral("/somewhere"));
        content.tag = QStringLiteral("a3f9c1");
        Hole old = media(1, 46100, true);
        old.old = true;
        content.holes = {tunnel(3478), tunnel(3478, true), streams(48550), old};
        Content back;
        CHECK(parse(serialize(content), false, back));
        CHECK(back.owner == content.owner && back.tag == content.tag);
        CHECK(back.holes == content.holes);

        // Anything that is not a record is none at all
        Content out;
        CHECK(!parse(QByteArray(), false, out));
        CHECK(!parse(QByteArrayLiteral("not json"), false, out));
        CHECK(!parse(QByteArrayLiteral("[]"), false, out));
        CHECK(!parse(QByteArrayLiteral("{}"), false, out));
        const auto rootAfter = [](const std::function<void(QJsonObject&)>& spoil) {
            QJsonObject root = wholeRecord();
            spoil(root);
            Content parsed;
            return parse(bytesOf(root), false, parsed);
        };
        CHECK(rootAfter([](QJsonObject&) {}));
        CHECK(!rootAfter([](QJsonObject& r) { r[QStringLiteral("version")] = 2; }));
        CHECK(
            !rootAfter([](QJsonObject& r) { r[QStringLiteral("version")] = QStringLiteral("1"); }));
        CHECK(!rootAfter([](QJsonObject& r) { r.remove(QStringLiteral("version")); }));
        CHECK(!rootAfter([](QJsonObject& r) { r.remove(QStringLiteral("owner")); }));
        CHECK(
            !rootAfter([](QJsonObject& r) { r[QStringLiteral("owner")] = QStringLiteral("me"); }));
        CHECK(!rootAfter([](QJsonObject& r) {
            r[QStringLiteral("owner")] = r.value(QStringLiteral("owner")).toString().toUpper();
        }));
        CHECK(!rootAfter(
            [](QJsonObject& r) { r[QStringLiteral("tag")] = QStringLiteral("A3F9C1"); }));
        CHECK(!rootAfter([](QJsonObject& r) { r[QStringLiteral("tag")] = 12; }));
        CHECK(!rootAfter([](QJsonObject& r) { r[QStringLiteral("holes")] = QJsonObject(); }));
        CHECK(!rootAfter([](QJsonObject& r) { r.remove(QStringLiteral("holes")); }));
        CHECK(!rootAfter([](QJsonObject& r) { r[QStringLiteral("more")] = 1; }));
        // More lines than a host has numbers
        CHECK(!rootAfter([](QJsonObject& r) {
            QJsonArray many;
            for (qsizetype i = 0; i <= kHolesMax; ++i)
                many.append(QJsonObject());
            r[QStringLiteral("holes")] = many;
        }));
        // A file heavier than a record can be
        CHECK(!parse(QByteArray(kFileMax + 1, ' ') + bytesOf(wholeRecord()), false, out));

        // A line that does not read as one is left out; the others stay
        const QList<Hole> rest{media(1, 46100, true)};
        CHECK(linesAfter([](QJsonObject&) {}).size() == 2);
        const auto key = [](const char* name) { return QString::fromLatin1(name); };
        CHECK(linesAfter([&](QJsonObject& l) { l[key("proto")] = QStringLiteral("udp"); }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("proto")] = QStringLiteral("SCTP"); }) ==
              rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("proto")] = 17; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("what")] = QStringLiteral("door"); }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("port")] = 0; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("port")] = 65536; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("port")] = 3478.5; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("port")] = QStringLiteral("3478"); }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("to")] = -1; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l.remove(key("to")); }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("lan")] = QStringLiteral("192.168.1"); }) ==
              rest);
        CHECK(linesAfter([&](QJsonObject& l) { l.remove(key("lan")); }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("old")] = 1; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("more")] = true; }) == rest);
        // A slot is a stream slot's hole's, and no other line's
        CHECK(linesAfter([&](QJsonObject& l) { l[key("slot")] = 0; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) { l[key("what")] = QStringLiteral("media"); }) ==
              rest);
        // A line this kind of instance may not have written: the tunnel
        // forwarded elsewhere, a number that is nobody's
        CHECK(linesAfter([&](QJsonObject& l) { l[key("to")] = 22; }) == rest);
        CHECK(linesAfter([&](QJsonObject& l) {
                  l[key("port")] = 443;
                  l[key("to")] = 443;
              }) == rest);
        // The same number twice in one protocol: the first is the line
        {
            QJsonObject root = wholeRecord();
            QJsonArray holes = root.value(QStringLiteral("holes")).toArray();
            QJsonObject again = holes.at(0).toObject();
            again[QStringLiteral("old")] = true;
            holes.append(again);
            root[QStringLiteral("holes")] = holes;
            Content parsed;
            CHECK(parse(bytesOf(root), false, parsed));
            CHECK(parsed.holes == (QList<Hole>{tunnel(3478), media(1, 46100, true)}));
        }
        // What the installed host wrote is not a DEV identity's to act on
        {
            Content parsed;
            CHECK(parse(bytesOf(wholeRecord()), true, parsed));
            CHECK(parsed.holes == rest);
        }
    }

    SECTION("RouterHoles: whose record it is");
    {
        QTemporaryDir one;
        QTemporaryDir two;
        CHECK(one.isValid() && two.isValid());
        const QString mine = fingerprint(kMachine, one.path());
        CHECK_EQ(mine.size(), 32);
        CHECK(mine == mine.toLower());
        CHECK(mine == fingerprint(kMachine, one.path()));
        // The same directory, written another way
        CHECK(mine == fingerprint(kMachine, one.path() + QStringLiteral("/.")));
        // Another directory of this machine, the same directory of another machine
        CHECK(mine != fingerprint(kMachine, two.path()));
        CHECK(mine != fingerprint(QByteArrayLiteral("machine-two"), one.path()));
        CHECK(mine != fingerprint(QByteArray(), one.path()));
    }

    SECTION("RouterHoles: what a version that kept no record remembers");
    {
        Earlier earlier;
        earlier.lan = kLan;
        earlier.tunnel = {3478, 46000, 443};
        earlier.media = {{0, 48550}, {2, 46100}, {3, 48010}, {30, 46101}};
        Hole first = tunnel(3478);
        first.old = true;
        const QList<Hole> lines = earlierHoles(false, earlier, kLan);
        // In UDP and in TCP, each as such a version wrote it
        CHECK_EQ(lines.size(), 8);
        CHECK(lines.at(0) == first);
        for (const Hole& line : lines) {
            CHECK(line.old && line.lan == kLan);
            CHECK(allowed(false, line));
        }
        Hole second = media(2, 46100, true);
        second.old = true;
        CHECK(lines.contains(second));
        // 443 is nobody's, 48010 is no slot's number any more, slot 30 is none
        for (const Hole& line : lines)
            CHECK(line.external != 443 && line.external != 48010 && line.external != 46101);
        // A DEV identity never had the installed host's tunnel numbers
        for (const Hole& line : earlierHoles(true, earlier, kLan))
            CHECK(line.kind == Kind::Media);
        // Obtained under another address than today's, or never: nothing
        CHECK(earlierHoles(false, earlier, QStringLiteral("192.168.1.21")).isEmpty());
        earlier.lan.clear();
        CHECK(earlierHoles(false, earlier, kLan).isEmpty());
        CHECK(earlierHoles(false, earlier, QString()).isEmpty());
    }

    SECTION("RouterHoles: the record on disk");
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("router_holes.json"));
        const Earlier none;
        QString said;

        // A first start, nothing remembered: an empty record with a tag
        auto first = File::open(dir.path(), false, kMachine, none, kLan, &said);
        CHECK(first != nullptr);
        if (!first) return;
        CHECK(said.isEmpty());
        const QString tag = first->tag();
        CHECK(validTag(tag));
        CHECK(first->holes().isEmpty());
        CHECK(QFile::exists(path));

        // While that process lives, the record is nobody else's
        auto second = File::open(dir.path(), false, kMachine, none, kLan, &said);
        CHECK(second == nullptr);
        CHECK(!said.isEmpty());

        // What is noted is on disk at once, and reads back as it was noted
        first->note(tunnel(3478));
        first->note(media(1, 46100, true));
        Content onDisk;
        CHECK(parse(readFile(path), false, onDisk));
        CHECK(onDisk.tag == tag && onDisk.owner == fingerprint(kMachine, dir.path()));
        CHECK(onDisk.holes == (QList<Hole>{tunnel(3478), media(1, 46100, true)}));
        // The same line again changes nothing; another of that number replaces it
        first->note(tunnel(3478));
        CHECK_EQ(first->holes().size(), 2);
        Hole renumbered = tunnel(3478);
        renumbered.lan = QStringLiteral("192.168.1.21");
        first->note(renumbered);
        Hole found;
        CHECK(first->find(3478, false, found) && found == renumbered);
        CHECK(!first->find(3478, true, found));
        first->note(tunnel(3478));
        first->strike(46100, true);
        first->strike(46100, true);
        CHECK(first->holes() == QList<Hole>{tunnel(3478)});

        // The process ends: the next one finds the tag and the line it left
        first.reset();
        said.clear();
        Earlier remembered;
        remembered.lan = kLan;
        remembered.tunnel = {3479};
        auto next = File::open(dir.path(), false, kMachine, remembered, kLan, &said);
        CHECK(next != nullptr);
        if (!next) return;
        CHECK(said.isEmpty());
        CHECK(next->tag() == tag);
        // What the settings remember stands for a record only where there was none
        CHECK(next->holes() == QList<Hole>{tunnel(3478)});
        next.reset();

        // The profile copied to another directory: not that installation's record
        QTemporaryDir copy;
        CHECK(copy.isValid());
        const QString copied = QDir(copy.path()).filePath(QStringLiteral("router_holes.json"));
        writeFile(copied, readFile(path));
        said.clear();
        auto other = File::open(copy.path(), false, kMachine, remembered, kLan, &said);
        CHECK(other != nullptr);
        if (!other) return;
        CHECK(!said.isEmpty());
        CHECK(other->holes().isEmpty());
        CHECK(validTag(other->tag()));
        Content replaced;
        CHECK(parse(readFile(copied), false, replaced));
        CHECK(replaced.owner == fingerprint(kMachine, copy.path()) && replaced.holes.isEmpty());
        other.reset();

        // The same directory on another machine: not that one's either
        said.clear();
        auto restored =
            File::open(dir.path(), false, QByteArrayLiteral("machine-two"), none, kLan, &said);
        CHECK(restored != nullptr);
        if (!restored) return;
        CHECK(!said.isEmpty());
        CHECK(restored->holes().isEmpty());
        restored.reset();

        // A file that does not read as a record, or is heavier than one
        for (const QByteArray& bad : {QByteArrayLiteral("{\"version\":1"), QByteArray(1, '\0'),
                                      QByteArray(kFileMax + 1, ' ')}) {
            writeFile(path, bad);
            said.clear();
            auto fresh = File::open(dir.path(), false, kMachine, remembered, kLan, &said);
            CHECK(fresh != nullptr);
            if (!fresh) return;
            CHECK(!said.isEmpty());
            CHECK(fresh->holes().isEmpty());
            Content now;
            CHECK(parse(readFile(path), false, now));
        }
    }
    // Where no record was ever kept, the numbers the settings remember are its first lines
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        Earlier remembered;
        remembered.lan = kLan;
        remembered.tunnel = {3478};
        remembered.media = {{1, 46100}};
        QString said;
        auto file = File::open(dir.path(), false, kMachine, remembered, kLan, &said);
        CHECK(file != nullptr);
        if (!file) return;
        CHECK_EQ(file->holes().size(), 4);
        for (const Hole& line : file->holes())
            CHECK(line.old);
        const QString tag = file->tag();
        file.reset();
        // …once: they stay lines of that version for as long as they are there
        remembered.tunnel = {3479};
        file = File::open(dir.path(), false, kMachine, remembered, kLan, &said);
        CHECK(file != nullptr);
        if (!file) return;
        CHECK(file->tag() == tag);
        CHECK_EQ(file->holes().size(), 4);
        Hole line;
        CHECK(file->find(3478, false, line) && line.old);
        CHECK(!file->find(3479, false, line));
    }
    // Obtained under another address: no line
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        Earlier remembered;
        remembered.lan = QStringLiteral("192.168.1.21");
        remembered.tunnel = {3478};
        QString said;
        auto file = File::open(dir.path(), false, kMachine, remembered, kLan, &said);
        CHECK(file != nullptr);
        if (!file) return;
        CHECK(file->holes().isEmpty());
    }
}
