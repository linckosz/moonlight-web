/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/Logger.h"
#include "common/ZipWriter.h"
#include "server/LogArchive.h"
#include "server/LogScrubber.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QThread>

namespace {

quint32 le32(const QByteArray& b, qsizetype at)
{
    return quint32(quint8(b[at])) | quint32(quint8(b[at + 1])) << 8 |
           quint32(quint8(b[at + 2])) << 16 | quint32(quint8(b[at + 3])) << 24;
}

quint16 le16(const QByteArray& b, qsizetype at)
{
    return quint16(quint8(b[at]) | quint8(b[at + 1]) << 8);
}

quint32 adler32(const QByteArray& data)
{
    quint32 a = 1, b = 0;
    for (const char c : data) {
        a = (a + quint8(c)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

struct Entry
{
    quint16 flags = 0;
    quint16 method = 0;
    quint32 crc = 0;
    quint32 size = 0;
    QByteArray raw;
};

// Every entry of an archive, read the way an unzip tool finds them: from the
// end record to the central directory, then to each local header. Empty when
// any signature is not where it should be.
QMap<QString, Entry> readZip(const QByteArray& zip)
{
    QMap<QString, Entry> out;
    if (zip.size() < 22) return {};
    const qsizetype eocd = zip.size() - 22;
    if (le32(zip, eocd) != 0x06054b50) return {};
    const int count = le16(zip, eocd + 10);
    qsizetype cd = le32(zip, eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (le32(zip, cd) != 0x02014b50) return {};
        Entry e;
        e.flags = le16(zip, cd + 8);
        e.method = le16(zip, cd + 10);
        e.crc = le32(zip, cd + 16);
        const quint32 csize = le32(zip, cd + 20);
        e.size = le32(zip, cd + 24);
        const quint16 nlen = le16(zip, cd + 28);
        const quint32 local = le32(zip, cd + 42);
        const QString name = QString::fromUtf8(zip.mid(cd + 46, nlen));
        if (le32(zip, local) != 0x04034b50) return {};
        const qsizetype body = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
        e.raw = zip.mid(body, csize);
        out.insert(name, e);
        cd += 46 + nlen + le16(zip, cd + 30) + le16(zip, cd + 32);
    }
    return out;
}

// Whether @p e holds exactly @p expected: stored as is, or deflated so that
// zlib inflates it to those bytes. qUncompress wants the zlib wrapper and its
// Adler-32, which only the expected bytes can give; zlib refuses the stream
// unless its output matches that checksum, and the CRC is checked on top.
bool holds(const Entry& e, const QByteArray& expected)
{
    if (e.size != quint32(expected.size()) || e.crc != ZipWriter::crc32(expected)) return false;
    if (e.method == 0) return e.raw == expected;
    if (e.method != 8) return false;
    QByteArray z;
    for (int i = 3; i >= 0; --i)
        z.append(char((e.size >> (8 * i)) & 0xFF));
    z.append(char(0x78));
    z.append(char(0x9C));
    z.append(e.raw);
    const quint32 adler = adler32(expected);
    for (int i = 3; i >= 0; --i)
        z.append(char((adler >> (8 * i)) & 0xFF));
    return qUncompress(z) == expected;
}

void touch(const QString& path, const QByteArray& data, const QDateTime& when)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.close();
    QFile g(path);
    g.open(QIODevice::ReadWrite);
    g.setFileTime(when, QFileDevice::FileModificationTime);
    g.close();
}

} // namespace

void run_log_scrubber_tests();

void run_log_archive_tests()
{
    SECTION("LogArchive / ZipWriter");

    // The check value every CRC-32 implementation is held to.
    CHECK_EQ(ZipWriter::crc32(QByteArray("123456789")), quint32(0xCBF43926));
    CHECK_EQ(ZipWriter::crc32(QByteArray()), quint32(0));
    CHECK(ZipWriter::rawDeflate(QByteArray()).isEmpty());

    // An archive read back as an unzip tool would: names (a Chinese one among
    // them) flagged UTF-8, bodies inflated to the same bytes, CRCs right; a
    // log compresses, an empty file is stored.
    {
        QByteArray log;
        for (int i = 0; i < 2000; ++i)
            log += "[2026-09-30 10:00:00.000] [INFO] [NETWORK] poll serverinfo — 日志 " +
                   QByteArray::number(i) + "\n";
        ZipWriter zip;
        CHECK(zip.addFile(QStringLiteral("about.txt"), "MoonlightWeb test\n",
                          QDateTime::currentDateTime()));
        CHECK(zip.addFile(QStringLiteral("服务器日志.log"), log, QDateTime::currentDateTime()));
        CHECK(zip.addFile(QStringLiteral("empty.log"), QByteArray(), QDateTime()));
        CHECK(!zip.addFile(QString(), "x", QDateTime::currentDateTime()));
        CHECK_EQ(zip.count(), 3);
        const QByteArray bytes = zip.finish();
        CHECK(bytes.size() < log.size() / 4); // deflate did its work
        const QMap<QString, Entry> entries = readZip(bytes);
        CHECK_EQ(entries.size(), qsizetype(3));
        for (const Entry& e : entries)
            CHECK(e.flags & (1 << 11)); // names flagged UTF-8
        CHECK(holds(entries.value(QStringLiteral("服务器日志.log")), log));
        CHECK_EQ(entries.value(QStringLiteral("服务器日志.log")).method, quint16(8));
        CHECK(holds(entries.value(QStringLiteral("about.txt")), "MoonlightWeb test\n"));
        CHECK(holds(entries.value(QStringLiteral("empty.log")), QByteArray()));
        CHECK_EQ(entries.value(QStringLiteral("empty.log")).method, quint16(0));
    }

    // Kinds: a log's archives are that log, every worker's file is one kind.
    CHECK_EQ(LogArchive::kindOf("moonlightweb.log"), QString("moonlightweb.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb.log.20260930-101010"), QString("moonlightweb.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb.log.20260930-101010-2"), QString("moonlightweb.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb-worker-13372.log"),
             QString("moonlightweb-worker-*.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb-worker-service.log"),
             QString("moonlightweb-worker-service.log"));
    CHECK_EQ(LogArchive::kindOf("moonlightweb-client.log.20260930-101010"),
             QString("moonlightweb-client.log"));
    // A bench's traces, one file per run: three kinds, not one per file.
    CHECK_EQ(LogArchive::kindOf("click-trace-13372-1793000000000.csv"),
             QString("click-trace-*.csv"));
    CHECK_EQ(LogArchive::kindOf("relay-frames-13372-1793000000000.csv"),
             QString("relay-frames-*.csv"));
    CHECK_EQ(LogArchive::kindOf("relay-audio-13372-1793000000000.csv"),
             QString("relay-audio-*.csv"));
    // Numbers that do not end the name, or no separator before them: its own name.
    CHECK_EQ(LogArchive::kindOf("internet-access-audit.log"), QString("internet-access-audit.log"));
    CHECK_EQ(LogArchive::kindOf("h264-capture.log"), QString("h264-capture.log"));
    CHECK_EQ(LogArchive::kindOf("trace2.csv"), QString("trace2.csv"));

    // Only the newest non-empty file of each kind, never the directory.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString dir = tmp.path();
    const QDateTime now = QDateTime::currentDateTime();
    touch(dir + "/moonlightweb.log", "live\n", now.addSecs(-10));
    touch(dir + "/moonlightweb.log.20260929-080000", "yesterday\n", now.addDays(-1));
    touch(dir + "/moonlightweb-worker-100.log", "old stream\n", now.addSecs(-3600));
    touch(dir + "/moonlightweb-worker-200.log", "last stream\n", now.addSecs(-60));
    touch(dir + "/moonlightweb-worker-300.log", "", now); // started, wrote nothing
    touch(dir + "/moonlightweb-probe.log", "probe\n", now.addDays(-3));
    touch(dir + "/moonlightweb-vdisplay.log", "", now.addDays(-2));
    {
        const QList<QFileInfo> picked =
            LogArchive::latestPerKind(QDir(dir).entryInfoList(QDir::Files));
        QStringList names;
        for (const QFileInfo& f : picked)
            names << f.fileName();
        CHECK_EQ(names, QStringList({"moonlightweb-probe.log", "moonlightweb-worker-200.log",
                                     "moonlightweb.log"}));
    }

    // The whole job, on its thread: about.txt plus those three, nothing else.
    {
        LogArchive archive;
        CHECK_EQ(archive.status().state, QString("idle"));
        CHECK(archive.start(dir, QString(), "about\n", "logs.zip"));
        QElapsedTimer t;
        t.start();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        const LogArchive::Status s = archive.status();
        CHECK_EQ(s.state, QString("done"));
        CHECK_EQ(s.files, 3);
        CHECK_EQ(s.doneBytes, s.totalBytes);
        CHECK_EQ(s.fileName, QString("logs.zip"));
        const QMap<QString, Entry> entries = readZip(archive.result());
        CHECK_EQ(entries.keys(), QStringList({"about.txt", "moonlightweb-probe.log",
                                              "moonlightweb-worker-200.log", "moonlightweb.log"}));
        CHECK(holds(entries.value("moonlightweb-worker-200.log"), "last stream\n"));
        CHECK(holds(entries.value("about.txt"), "about\n"));

        // A server logging elsewhere (--log): its file stands for the server's.
        QTemporaryDir other;
        touch(other.path() + "/dev-run.log", "the running server\n", now);
        CHECK(archive.start(dir, other.path() + "/dev-run.log", "about\n", "logs.zip"));
        t.restart();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        const QMap<QString, Entry> again = readZip(archive.result());
        CHECK(again.contains("dev-run.log"));
        CHECK(!again.contains("moonlightweb.log"));
    }

    // A folder a bench has filled: the last trace of each sort goes in, and the
    // others are not even read (1.5 GB in 6,000 files on the machine that showed it).
    {
        QTemporaryDir bench;
        CHECK(bench.isValid());
        const QString d = bench.path();
        touch(d + "/moonlightweb.log", "server\n", now);
        for (int i = 0; i < 50; ++i) {
            const QString run = QStringLiteral("-%1-%2.csv").arg(1000 + i).arg(1793000000000LL + i);
            const QDateTime when = now.addSecs(-3600 + i);
            const QByteArray last = i == 49 ? "newest\n" : "older\n";
            touch(d + "/click-trace" + run, last, when);
            touch(d + "/relay-frames" + run, last, when);
            touch(d + "/relay-audio" + run, last, when);
        }
        LogArchive archive;
        CHECK(archive.start(d, QString(), "about\n", "logs.zip"));
        QElapsedTimer t;
        t.start();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        const LogArchive::Status s = archive.status();
        CHECK_EQ(s.state, QString("done"));
        CHECK_EQ(s.files, 4);
        const QMap<QString, Entry> entries = readZip(archive.result());
        CHECK_EQ(entries.size(), qsizetype(5)); // about.txt too
        const QString lastRun = QStringLiteral("-1049-%1.csv").arg(1793000000049LL);
        CHECK(holds(entries.value("click-trace" + lastRun), "newest\n"));
        CHECK(holds(entries.value("relay-frames" + lastRun), "newest\n"));
        CHECK(holds(entries.value("relay-audio" + lastRun), "newest\n"));
    }

    // A log still open for writing, as the client log always is: on Windows
    // the directory listing says 0 bytes until its writer closes it, and the
    // archive left it out (issue #48: a Tesla's console, never in the zip).
    {
        QTemporaryDir live;
        CHECK(live.isValid());
        QFile writer(live.path() + "/moonlightweb-client.log");
        CHECK(writer.open(QIODevice::WriteOnly | QIODevice::Append));
        const QByteArray lines = "[Caps] browser {}\n[Life] visibility hidden\n";
        CHECK_EQ(writer.write(lines), qint64(lines.size()));
        CHECK(writer.flush()); // written, as the Logger does: no FlushFileBuffers
        const QList<QFileInfo> picked =
            LogArchive::latestPerKind(QDir(live.path()).entryInfoList(QDir::Files));
        CHECK_EQ(picked.size(), qsizetype(1));
        if (!picked.isEmpty()) CHECK_EQ(picked.first().size(), qint64(lines.size()));

        LogArchive archive;
        CHECK(archive.start(live.path(), QString(), "about\n", "logs.zip"));
        QElapsedTimer t;
        t.start();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        CHECK_EQ(archive.status().state, QString("done"));
        CHECK_EQ(archive.status().doneBytes, qint64(lines.size()));
        CHECK(holds(readZip(archive.result()).value("moonlightweb-client.log"), lines));
        writer.close();
    }

    // The Logger's threshold: DEBUG dropped unless asked for.
    {
        Logger* log = Logger::client();
        log->setLogFile(dir + "/threshold.log", /*rotating=*/false);
        log->setMinLevel(Logger::Info);
        log->log(Logger::Debug, "hidden line");
        log->log(Logger::Info, "shown line");
        log->setMinLevel(Logger::Debug);
        log->log(Logger::Debug, "verbose line");
        QFile f(dir + "/threshold.log");
        f.open(QIODevice::ReadOnly);
        const QByteArray text = f.readAll();
        CHECK(!text.contains("hidden line"));
        CHECK(text.contains("shown line"));
        CHECK(text.contains("verbose line"));
        CHECK_EQ(Logger::instance()->minLevel(), Logger::Info); // the default
    }

    run_log_scrubber_tests();
}

// What the archive must never carry to a public issue, in the log's own
// formats, and what it must leave readable.
void run_log_scrubber_tests()
{
    SECTION("LogScrubber");

    LogScrubber::Names names;
    // This PC: its LAN card, VirtualBox, Tailscale (v4 and v6) and a Hyper-V
    // switch; one host with its LAN address.
    names.thisMachine = {{"DUALRTX"},
                         {"192.168.1.66", "192.168.56.1", "100.116.41.43", "172.24.208.1",
                          "fd7a:115c:a1e0::c036:292d"}};
    names.hosts = {{{"DualRTX"}, {}},
                   {{"Luka-MacBook-Pro"}, {}},
                   {{"leos-macbook-pro2.home"}, {}},
                   {{"Wolf"}, {}},
                   {{"UM790Pro"}, {"192.168.1.9"}}};
    names.instance = "Salon";
    names.user = "bruno";
    LogScrubber s(names);

    // Secrets: rikey and the client id through Qt's error string, which quotes
    // the whole URL; the pairing handshake.
    {
        const QString out = s.scrubLine(
            R"~([Session] Launch failed: "Error transferring https://192.168.1.5:47984/launch?appid=1&uniqueid=0123456789ABCDEF&uuid=a1b2&mode=1920x1080x60&rikey=00112233445566778899aabbccddeeff&rikeyid=12345 - server replied: Service Unavailable" kind= 3)~");
        CHECK(!out.contains("00112233445566778899aabbccddeeff"));
        CHECK(!out.contains("0123456789ABCDEF"));
        CHECK(!out.contains("12345 "));
        CHECK(out.contains("rikey=(hidden)&rikeyid=(hidden)"));
        CHECK(out.contains("https://192.168.x.{a1}:47984/launch?appid=1&uniqueid=(hidden)"));
        CHECK(out.contains("mode=1920x1080x60"));
        const QString pair = s.scrubLine(
            "Pairing request failed: http pair → Error transferring http://10.0.0.2:47989/"
            "pair?devicename=roth&updateState=1&phrase=getservercert&salt=a1b2c3d4&clientcert="
            "2d2d2d2d2d424547 - server replied: Bad Request");
        CHECK(!pair.contains("a1b2c3d4"));
        CHECK(!pair.contains("2d2d2d2d2d424547"));
        CHECK(!pair.contains("roth"));
        CHECK(pair.contains("phrase=getservercert"));
        CHECK_EQ(s.scrubLine("<root><pairingsecret>ABCDEF0123</pairingsecret><paired>1</paired>"),
                 QString("<root><pairingsecret>(hidden)</pairingsecret><paired>1</paired>"));
    }
    CHECK_EQ(s.scrubLine("[Auth] PIN changed: 482913"), QString("[Auth] PIN changed: (hidden)"));
    CHECK_EQ(s.scrubLine("Pairing initiated for BCA1-22, PIN: 0427"),
             QString("Pairing initiated for BCA1-22, PIN: (hidden)"));
    CHECK_EQ(s.scrubLine("[Auth] Revoke request — token='AbC_dEf-123', size=43"),
             QString("[Auth] Revoke request — token='(hidden)', size=43"));
    CHECK_EQ(s.scrubLine("[IdentityManager] Using Moonlight common unique ID: 0123ABCD4567"),
             QString("[IdentityManager] Using Moonlight common unique ID: (hidden)"));
    CHECK_EQ(s.scrubLine(R"~([main] Revoke teardown (uid= "F1E2D3C4B5" ))~"),
             QString(R"~([main] Revoke teardown (uid= "(hidden)" ))~"));
    // ICE credentials, from libjuice in verbose mode and from a candidate.
    CHECK_EQ(s.scrubLine(R"~([rtc] juice: STUN integrity check failed, password="s3cr3tpwd")~"),
             QString(R"~([rtc] juice: STUN integrity check failed, password="(hidden)")~"));
    CHECK_EQ(s.scrubLine(R"~(STUN local ufrag check failed, expected="abcd", actual="efgh")~"),
             QString(R"~(STUN local ufrag check failed, expected="(hidden)", actual="(hidden)")~"));
    CHECK(!s.scrubLine("candidate:1 1 udp 2122 192.168.1.9 5000 typ host ufrag Xy12 network-id 1")
               .contains("Xy12"));
    CHECK_EQ(s.scrubLine("a=ice-pwd:abcdefghijklmnop"), QString("a=ice-pwd:(hidden)"));
    CHECK_EQ(s.scrubLine("Cookie: mw_session=abc; mw_player=def"), QString("Cookie: (hidden)"));
    CHECK_EQ(s.scrubLine("[Tunnel] Ready — host key C1:EC:02:3F:44:BE:05:AC:66:7A:C8:E9:E0"),
             QString("[Tunnel] Ready — host key (hidden)"));
    // Keystrokes: hidden, the line kept; the switch's own line stays.
    CHECK_EQ(s.scrubLine("[KBD] KeyA client 'a' (non-US) -> position VK 0x41 | Notepad: a"),
             QString("[KBD] (keystroke hidden)"));
    CHECK_EQ(s.scrubLine("[KBD] keyboard diagnostics on — one line per printable key press"),
             QString("[KBD] keyboard diagnostics on — one line per printable key press"));
    CHECK_EQ(s.scrubLine("[StreamView] Input gate closed (focus): Bank statement.pdf - Viewer"),
             QString("[StreamView] Input gate closed (focus): (hidden)"));
    CHECK_EQ(s.scrubLine("[Auth] Geo data stored for session abcd1234: Lyon, France"),
             QString("[Auth] Geo data stored for session abcd1234: (hidden)"));
    CHECK_EQ(s.scrubLine("Wake-on-LAN sent to host (aa:bb:cc:dd:ee:ff)"),
             QString("Wake-on-LAN sent to host ((hidden MAC))"));
    CHECK(!s.scrubLine("contact: someone.else@example.org").contains("example.org"));

    // Links that open the instance; the project's own names stay.
    CHECK_EQ(
        s.scrubLine(
            R"~([RDV] line up — "https://stream.moonlightweb.top/41zvnyqjdnybgwhw8f20martz4")~"),
        QString(R"~([RDV] line up — "https://stream.moonlightweb.top/(link)")~"));
    CHECK_EQ(s.scrubLine("  From the internet: https://stream.dev.moonlightweb.top/wwj7jmmpb"),
             QString("  From the internet: https://stream.dev.moonlightweb.top/(link)"));
    CHECK_EQ(s.scrubLine("[k3j4 guest 2@192.168.1.20] page /p/Zx81-abcdefgh — Mozilla/5.0"),
             QString("[k3j4 guest 2@192.168.x.{a2}] page /p/(link) — Mozilla/5.0"));
    CHECK_EQ(s.scrubLine("[k3j4 owner@192.168.1.20] page /41zvnyqjdnybgwhw8f20martz4 — x"),
             QString("[k3j4 owner@192.168.x.{a2}] page /(link) — x"));
    CHECK_EQ(s.scrubLine("SSL certificate loaded from source: CN=8f3b2aa.moonlightweb.top"),
             QString("SSL certificate loaded from source: CN=instance.moonlightweb.top"));
    CHECK_EQ(s.scrubLine("Untrusted Host 'dualrtx.tailbea5b3.ts.net' refused"),
             QString("Untrusted Host '{A}.tailnet.ts.net' refused"));

    // Machines: one capital letter each, by name and by address, {A} being
    // this PC; letters given in order of appearance (UM790Pro's address came
    // first, in the candidate line: {B}); generic words left alone.
    CHECK_EQ(s.scrubLine("[NETWORK] DualRTX is online (192.168.1.12:47989)"),
             QString("[NETWORK] {A} is online (192.168.x.{a3}:47989)"));
    CHECK_EQ(s.scrubLine("[NETWORK] UM790Pro is online (192.168.1.9:47989)"),
             QString("[NETWORK] {B} is online (192.168.x.{B}:47989)"));
    CHECK_EQ(s.scrubLine("Host updated: Luka-MacBook-Pro, then luka-macbook-pro again"),
             QString("Host updated: {C}, then {C} again"));
    CHECK_EQ(s.scrubLine("mDNS host discovered: leos-macbook-pro2.local."),
             QString("mDNS host discovered: {D}.local."));
    CHECK_EQ(s.scrubLine("[WolfApi] Wolf answered; UM790Pro did not"),
             QString("[WolfApi] Wolf answered; {B} did not"));
    // A machine the host list does not know, by its LAN name, is a machine
    // too; Chrome's random mDNS names stay.
    CHECK_EQ(s.scrubLine("mDNS host discovered: Kids-iPad.local. (and kids-ipad.lan) "
                         "f6636672-ff76-4595-8733-2a171e4fcb18.local"),
             QString("mDNS host discovered: {E}.local. (and {E}.lan) "
                     "f6636672-ff76-4595-8733-2a171e4fcb18.local"));
    // This PC's own interfaces are {A}, a second one under the same prefix
    // {A2}; an address of no known machine is a small letter and a number.
    CHECK_EQ(
        s.scrubLine(
            R"~([InternetAccess] Local LAN IP: "192.168.1.66" — all reachable: QList("192.168.1.66", "100.116.41.43", "192.168.56.1", "172.24.208.1", "172.29.128.1"))~"),
        QString(
            R"~([InternetAccess] Local LAN IP: "192.168.x.{A}" — all reachable: QList("192.168.x.{A}", "100.x.x.{A}", "192.168.x.{A2}", "172.24.x.{A}", "172.29.x.{c1}"))~"));
    CHECK_EQ(
        s.scrubLine(
            R"~(Selected candidate pair: local "host fd7a:115c:a1e0::c036:292d:48550/UDP" -> remote "prflx 192.168.1.20:50000/UDP")~"),
        QString(
            R"~(Selected candidate pair: local "host fd::{A}:48550/UDP" -> remote "prflx 192.168.x.{a2}:50000/UDP")~"));
    CHECK_EQ(s.scrubLine("[Settings] instance name: Salon"),
             QString("[Settings] instance name: instance-name"));
    CHECK_EQ(s.scrubLine("[CERT] Found private key: file=C:/Users/bruno/AppData/cert/key.pem"),
             QString("[CERT] Found private key: file=C:/Users/user/AppData/cert/key.pem"));
    CHECK_EQ(s.scrubLine(R"~(dir "C:\\Users\\Mimi\\AppData" and /home/minis/x and /Users/leo/y)~"),
             QString(R"~(dir "C:\\Users\\user\\AppData" and /home/user/x and /Users/user/y)~"));
    CHECK_EQ(s.scrubLine("Worker spawned in the console session as \"bruno (elevated)\""),
             QString("Worker spawned in the console session as \"user (elevated)\""));

    // Addresses: public ones by a number under a prefix no address has; LAN,
    // CGNAT, link-local and ULA ones of no known machine by a small letter
    // per subnet and a number, under the range's own octets; loopback kept;
    // versions and clocks are not addresses. Subnets a (192.168.1), b
    // (10.0.0) and c (172.29.128) come from the lines above.
    CHECK_EQ(s.scrubLine("[UPNP] External IP address: 88.12.34.56"),
             QString("[UPNP] External IP address: 256.x.x.{1}"));
    CHECK_EQ(s.scrubLine(R"~(Public IP changed from "88.12.34.56" to "90.1.2.3")~"),
             QString(R"~(Public IP changed from "256.x.x.{1}" to "256.x.x.{2}")~"));
    CHECK_EQ(
        s.scrubLine("peers 10.0.0.1 172.20.1.1 127.0.0.1 100.101.1.2 169.254.3.4 0.3.1.22"),
        QString("peers 10.x.x.{b2} 172.20.x.{d1} 127.0.0.1 100.x.x.{e1} 169.254.x.{f1} 0.3.1.22"));
    CHECK_EQ(s.scrubLine("[2026-09-30 10:00:00.123] [INFO] Chrome/153.0.0.0 Safari/537.36"),
             QString("[2026-09-30 10:00:00.123] [INFO] Chrome/153.0.0.0 Safari/537.36"));
    CHECK_EQ(
        s.scrubLine(R"~(candidate:1 1 UDP 2122 3fff:abc:abc:de0:aaaa:bbbb:1:2 5000 typ host)~"),
        QString(R"~(candidate:1 1 UDP 2122 3xxx::{3} 5000 typ host)~"));
    CHECK_EQ(s.scrubLine("from [3fff:abc:abc:de0:aaaa:bbbb:1:2]:443 and fd12:3456::c1 fe80::1 ::1"),
             QString("from [3xxx::{3}]:443 and fd::{g1} fe80::{h1} ::1"));
    CHECK_EQ(s.scrubLine("mapped ::ffff:88.12.34.56 at 12:34:56"),
             QString("mapped ::ffff:256.x.x.{1} at 12:34:56"));

    // With no machine known, who talks to whom still reads: a small letter
    // per subnet, a number per address, the network and the broadcast keeping
    // theirs; past z the letters go on as aa, ab...
    {
        LogScrubber lan;
        CHECK_EQ(lan.scrubLine("192.168.1.66 -> 192.168.1.9, then 192.168.1.66 again"),
                 QString("192.168.x.{a1} -> 192.168.x.{a2}, then 192.168.x.{a1} again"));
        CHECK_EQ(lan.scrubLine("SSDP 192.168.1.255 on 192.168.1.0/24 via 239.255.255.250"),
                 QString("SSDP 192.168.x.{a255} on 192.168.x.{a0}/24 via 239.255.255.250"));
        CHECK_EQ(lan.scrubLine("100.116.41.43 192.168.56.1 172.24.208.1 172.29.128.1"),
                 QString("100.x.x.{b1} 192.168.x.{c1} 172.24.x.{d1} 172.29.x.{e1}"));
        QString last;
        for (int i = 0; i < 21; ++i) // f to z
            last = lan.scrubLine(QStringLiteral("10.0.%1.7").arg(i));
        CHECK_EQ(last, QString("10.x.x.{z1}"));
        CHECK_EQ(lan.scrubLine("10.0.21.7 10.0.22.7"), QString("10.x.x.{aa1} 10.x.x.{ab1}"));
    }
    // IPv6 with its port and no brackets, the way libdatachannel writes a pair.
    {
        LogScrubber pair;
        CHECK_EQ(
            pair.scrubLine(
                R"~(Selected candidate pair: local "host fd7a:115c:a1e0::c036:292d:48550/UDP" -> remote "prflx fd7a:115c:a1e0::c036:292d:63433/UDP")~"),
            QString(
                R"~(Selected candidate pair: local "host fd::{a1}:48550/UDP" -> remote "prflx fd::{a1}:63433/UDP")~"));
        CHECK_EQ(pair.scrubLine("srflx 3fff:abc:abc:de0:aaaa:bbbb:1:2:5000/UDP at 10:00:00.123"),
                 QString("srflx 3xxx::{1}:5000/UDP at 10:00:00.123"));
    }

    // Whole files: PEM blocks go, line endings stay.
    CHECK_EQ(LogScrubber().scrub("a\r\n-----BEGIN PRIVATE KEY-----\r\nMIIE\r\n-----END PRIVATE "
                                 "KEY-----\r\nb\n"),
             QByteArray("a\r\n-----BEGIN PRIVATE KEY----- (hidden) -----END PRIVATE KEY-----"
                        "\r\nb\n"));

    // In the archive: every file and about.txt, one scrubber for all of them.
    {
        QTemporaryDir tmp;
        const QByteArray log = "[Auth] PIN changed: 482913\n"
                               "[UPNP] External IP address: 88.12.34.56\n"
                               "[NETWORK] UM790Pro is online (192.168.1.9:47989)\n";
        touch(tmp.path() + "/moonlightweb.log", log, QDateTime::currentDateTime());
        LogArchive archive;
        CHECK(archive.start(tmp.path(), QString(), "Settings file: /home/bruno/s.json\n",
                            "logs.zip", LogScrubber(names)));
        QElapsedTimer t;
        t.start();
        while (archive.status().state == QLatin1String("running") && t.elapsed() < 10000)
            QThread::msleep(10);
        CHECK_EQ(archive.status().state, QString("done"));
        const QMap<QString, Entry> entries = readZip(archive.result());
        const QByteArray expected = "[Auth] PIN changed: (hidden)\n"
                                    "[UPNP] External IP address: 256.x.x.{1}\n"
                                    "[NETWORK] {B} is online (192.168.x.{B}:47989)\n";
        CHECK(holds(entries.value("moonlightweb.log"), expected));
        CHECK(holds(entries.value("about.txt"), "Settings file: /home/user/s.json\n"));
    }
}
