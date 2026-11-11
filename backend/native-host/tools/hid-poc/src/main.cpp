/*
 * MoonlightWeb — native capture & encoding engine: lab tools.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

// mw-hid-poc — the HID passthrough's first proof (plan H2): a page reads a
// device through WebHID and sends its collections and reports here; this tool
// rebuilds the descriptor and creates the same device on this host.
//
//   mw-hid-poc [--port 39920] [--bind 0.0.0.0]   serve hid-probe.html's "Transmettre"
//   mw-hid-poc --selftest dump.json              encode a hid-probe reading, no device
//   mw-hid-poc --create dump.json [--hold 120]   create its devices without a page
//   --hwid none|usb|ours|<id>                    the hardware ids a Windows device gets
//   --ffb                                        the host's PID block, as for force feedback
//
// Protocol, one WebSocket per page:
//   page → tool, text  {"type":"attach","slot":n,"vendorId","productId","productName",
//                       "collections":[…]}  ·  {"type":"detach","slot":n}  ·  {"type":"ping","t":…}
//   page → tool, binary [slot u8][reportId u8][seq u16 LE][report bytes, id not included]
//   tool → page, text  hello · attached {slot, descriptor (hex), reports, nodes} ·
//                       refused {slot, why} · request {slot, kind, reportType, reportId, data} ·
//                       stats {slot, received, lost, injectUs {median, p95}} · pong {t}

#include "VirtualHid.h"
#include "streaming/HidCollectionsJson.h"
#include "input/HidDescriptor.h"
#include "input/HidPid.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <cstdio>
#include <map>

namespace hid = mw::native::input::hid;

namespace {

/// --ffb: build as the host does for a page that plays force feedback.
bool g_ffb = false;

QString hexOf(const std::vector<uint8_t>& bytes)
{
    QString s;
    for (uint8_t b : bytes)
        s += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
    return s.trimmed();
}

const char* kindName(hid::Kind k)
{
    return k == hid::Kind::Input ? "input" : k == hid::Kind::Output ? "output" : "feature";
}

/// The reports a descriptor declares, with their size on the wire.
QJsonArray reportsOf(const hid::Parsed& p)
{
    QJsonArray out;
    std::map<std::pair<int, int>, bool> seen;
    for (const hid::Field& f : p.fields) {
        if (!seen.emplace(std::make_pair(int(f.kind), int(f.reportId)), true).second) continue;
        out.append(
            QJsonObject{{QStringLiteral("kind"), QLatin1String(kindName(f.kind))},
                        {QStringLiteral("id"), f.reportId},
                        {QStringLiteral("bytes"), int(hid::reportBytes(p, f.kind, f.reportId))}});
    }
    return out;
}

/// --hwid: "usb" and "ours" are the two forms the PnP bench compared (20/10),
/// anything else is taken as it is. The first id names the device, so it needs
/// an enumerator: without one, no device at all. VHF puts its own
/// HID_DEVICE_SYSTEM_VHF, the id hidvhf.inf installs, before them.
QStringList hardwareIdsFor(const QString& form, uint16_t vendorId, uint16_t productId)
{
    if (form.isEmpty() || form == QLatin1String("none")) return {};
    const QString vp = QStringLiteral("VID_%1&PID_%2")
                           .arg(vendorId, 4, 16, QLatin1Char('0'))
                           .arg(productId, 4, 16, QLatin1Char('0'))
                           .toUpper();
    if (form == QLatin1String("usb")) return {QStringLiteral("VHF\\") + vp};
    if (form == QLatin1String("ours")) return {QStringLiteral("VHF\\MOONLIGHTWEB_VHID&") + vp};
    // Ours, then the one VHF gives when given none, which its collections then still carry.
    if (form == QLatin1String("both"))
        return {QStringLiteral("VHF\\MOONLIGHTWEB_VHID&") + vp, QStringLiteral("VHF\\") + vp};
    return form.split(QLatin1Char(','));
}

uint16_t idOf(const QJsonValue& v)
{
    return static_cast<uint16_t>(v.isString() ? v.toString().toUInt(nullptr, 16) : v.toInt());
}

struct Built
{
    std::vector<uint8_t> descriptor;
    hid::Parsed parsed;
    QString why; // empty when the host may create it
};

Built build(const QJsonArray& collections)
{
    Built b;
    QString error;
    std::vector<hid::Collection> c = collectionsFromJson(collections, &error);
    if (!error.isEmpty()) {
        b.why = QStringLiteral("unreadable collections: ") + error;
        return b;
    }
    hid::repairBounds(c);
    hid::repairUsages(c);
    // As the host's attach() does: the device's own PID block out, the
    // host's in with --ffb.
    const uint8_t pidId = g_ffb ? hid::pidFirstId(c) : 0;
    hid::stripPid(c);
    b.descriptor = pidId ? hid::encodeWithPid(c, pidId) : hid::encode(c);
    b.parsed = hid::parse(b.descriptor);
    b.why = QString::fromStdString(hid::validate(b.descriptor));
    return b;
}

/// --selftest: every device of a hid-probe reading, encoded and checked
/// against the report sizes the reading actually received.
int selftest(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(path));
        return 2;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    int bad = 0;
    for (const QJsonValue& dv : root.value(QStringLiteral("devices")).toArray()) {
        const QJsonObject d = dv.toObject();
        const Built b = build(d.value(QStringLiteral("collections")).toArray());
        std::printf("== %s %s:%s\n", qPrintable(d.value(QStringLiteral("productName")).toString()),
                    qPrintable(d.value(QStringLiteral("vid")).toString()),
                    qPrintable(d.value(QStringLiteral("pid")).toString()));
        std::printf("   descriptor %zu bytes: %s\n", b.descriptor.size(),
                    qPrintable(hexOf(b.descriptor)));
        std::printf("   validate: %s\n", b.why.isEmpty() ? "accepted" : qPrintable(b.why));
        for (const QJsonValue& r : reportsOf(b.parsed)) {
            const QJsonObject o = r.toObject();
            std::printf("   %s report %d: %d bytes\n",
                        qPrintable(o.value(QStringLiteral("kind")).toString()),
                        o.value(QStringLiteral("id")).toInt(),
                        o.value(QStringLiteral("bytes")).toInt());
        }
        // WebHID hands the page a report without its id byte.
        const bool ids = !(b.parsed.reportIds.size() == 1 && b.parsed.reportIds.front() == 0);
        for (const QJsonValue& rv : d.value(QStringLiteral("reports")).toArray()) {
            const QJsonObject r = rv.toObject();
            const int id = r.value(QStringLiteral("reportId")).toInt();
            const int got = r.value(QStringLiteral("bytes")).toInt();
            const int want =
                int(hid::reportBytes(b.parsed, hid::Kind::Input, uint8_t(id))) - (ids ? 1 : 0);
            const bool ok = got == want;
            bad += ok ? 0 : 1;
            std::printf("   received report %d: %d bytes, descriptor says %d %s\n", id, got, want,
                        ok ? "ok" : "MISMATCH");
        }
        if (!b.why.isEmpty()) {
            // A vendor-only interface is meant to be refused; anything else counts.
            bool vendorOnly = true;
            for (const QJsonValue& c : d.value(QStringLiteral("collections")).toArray())
                vendorOnly =
                    vendorOnly && c.toObject().value(QStringLiteral("usagePage")).toInt() >= 0xFF00;
            if (!vendorOnly) ++bad;
        }
    }
    std::printf("%s\n", bad ? "selftest FAILED" : "selftest ok");
    return bad ? 1 : 0;
}

/// --create: every accepted device of a hid-probe reading, made at once with
/// one neutral report each and held for `holdS` seconds, so the PnP bench can
/// read what the OS made of it without a page.
int createFrom(const QString& path, const QString& hwid, int holdS)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(path));
        return 2;
    }
    if (!VirtualHid::unavailableReason().isEmpty()) {
        std::fprintf(stderr, "%s\n", qPrintable(VirtualHid::unavailableReason()));
        return 2;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    std::vector<std::unique_ptr<VirtualHid>> devices;
    for (const QJsonValue& dv : root.value(QStringLiteral("devices")).toArray()) {
        const QJsonObject d = dv.toObject();
        const Built b = build(d.value(QStringLiteral("collections")).toArray());
        VirtualHidIdentity id;
        id.name = d.value(QStringLiteral("productName")).toString();
        id.vendorId = idOf(d.value(QStringLiteral("vid")));
        id.productId = idOf(d.value(QStringLiteral("pid")));
        id.descriptor = b.descriptor;
        id.hardwareIds = hardwareIdsFor(hwid, id.vendorId, id.productId);
        if (!b.why.isEmpty()) {
            std::printf("%s refused: %s\n", qPrintable(id.name), qPrintable(b.why));
            continue;
        }
        std::unique_ptr<VirtualHid> dev = VirtualHid::make();
        QString error;
        if (!dev->create(id, &error)) {
            std::printf("%s not created: %s\n", qPrintable(id.name), qPrintable(error));
            continue;
        }
        const bool numbered = !(b.parsed.reportIds.size() == 1 && b.parsed.reportIds.front() == 0);
        for (uint8_t rid : b.parsed.reportIds) {
            const size_t n = hid::reportBytes(b.parsed, hid::Kind::Input, rid);
            if (!n) continue;
            QByteArray report(int(n), '\0');
            if (numbered) report[0] = char(rid);
            dev->input(report);
        }
        std::printf("%s %04x:%04x created, descriptor %zu bytes, hardware ids [%s]\n",
                    qPrintable(id.name), id.vendorId, id.productId, b.descriptor.size(),
                    qPrintable(id.hardwareIds.join(QStringLiteral(", "))));
        devices.push_back(std::move(dev));
    }
    std::printf("holding %d s\n", holdS);
    std::fflush(stdout);
    if (devices.empty()) return 1;
    QTimer::singleShot(holdS * 1000, qApp, [] { QCoreApplication::exit(0); });
    const int rc = QCoreApplication::exec();
    devices.clear();
    std::printf("devices destroyed\n");
    return rc;
}

struct Slot
{
    std::unique_ptr<VirtualHid> device;
    bool numbered = false;
    int received = 0;
    int lost = 0;
    int lastSeq = -1;
    std::vector<double> injectUs;
};

class Server : public QObject
{
public:
    Server(quint16 port, const QHostAddress& bind, const QString& hwid)
        : m_server(QStringLiteral("mw-hid-poc"), QWebSocketServer::NonSecureMode, this)
        , m_hwid(hwid)
    {
        if (!m_server.listen(bind, port)) {
            std::fprintf(stderr, "cannot listen on %s:%u: %s\n", qPrintable(bind.toString()), port,
                         qPrintable(m_server.errorString()));
            QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(1); });
            return;
        }
        std::printf("mw-hid-poc listening on ws://%s:%u — %s\n", qPrintable(bind.toString()), port,
                    VirtualHid::unavailableReason().isEmpty()
                        ? "devices will be created"
                        : qPrintable(VirtualHid::unavailableReason()));
        std::fflush(stdout);
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] { accept(); });
        auto* tick = new QTimer(this);
        connect(tick, &QTimer::timeout, this, [this] { stats(); });
        tick->start(5000);
    }

private:
    void accept()
    {
        // One page at a time: a second one replaces the first and its devices.
        QWebSocket* ws = m_server.nextPendingConnection();
        if (m_page) m_page->close();
        m_slots.clear();
        m_page = ws;
        std::printf("page connected from %s\n", qPrintable(ws->peerAddress().toString()));
        connect(ws, &QWebSocket::textMessageReceived, this,
                [this](const QString& t) { onText(t); });
        connect(ws, &QWebSocket::binaryMessageReceived, this,
                [this](const QByteArray& b) { onBinary(b); });
        connect(ws, &QWebSocket::disconnected, this, [this, ws] {
            if (m_page != ws) return;
            std::printf("page gone, devices destroyed\n");
            m_slots.clear();
            m_page = nullptr;
            ws->deleteLater();
        });
        send({{QStringLiteral("type"), QStringLiteral("hello")},
              {QStringLiteral("tool"), QStringLiteral("mw-hid-poc")},
              {QStringLiteral("canCreate"), VirtualHid::unavailableReason().isEmpty()},
              {QStringLiteral("why"), VirtualHid::unavailableReason()}});
    }

    void send(const QJsonObject& o)
    {
        if (m_page)
            m_page->sendTextMessage(
                QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    }

    void onText(const QString& text)
    {
        const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString type = o.value(QStringLiteral("type")).toString();
        const int slot = o.value(QStringLiteral("slot")).toInt(-1);
        if (type == QLatin1String("ping")) {
            send({{QStringLiteral("type"), QStringLiteral("pong")},
                  {QStringLiteral("t"), o.value(QStringLiteral("t"))}});
        } else if (type == QLatin1String("detach")) {
            m_slots.erase(slot);
        } else if (type == QLatin1String("attach") && slot >= 0 && slot < 16) {
            attach(slot, o);
        }
    }

    void attach(int slot, const QJsonObject& o)
    {
        m_slots.erase(slot);
        const Built b = build(o.value(QStringLiteral("collections")).toArray());
        const QString name = o.value(QStringLiteral("productName")).toString();
        auto refuse = [&](const QString& why) {
            std::printf("slot %d %s refused: %s\n", slot, qPrintable(name), qPrintable(why));
            std::fflush(stdout);
            send({{QStringLiteral("type"), QStringLiteral("refused")},
                  {QStringLiteral("slot"), slot},
                  {QStringLiteral("why"), why},
                  {QStringLiteral("descriptor"), hexOf(b.descriptor)}});
        };
        if (!b.why.isEmpty()) return refuse(b.why);
        std::unique_ptr<VirtualHid> dev = VirtualHid::make(this);
        if (!dev) return refuse(VirtualHid::unavailableReason());
        VirtualHidIdentity id;
        id.name = name;
        id.vendorId = static_cast<uint16_t>(o.value(QStringLiteral("vendorId")).toInt());
        id.productId = static_cast<uint16_t>(o.value(QStringLiteral("productId")).toInt());
        id.descriptor = b.descriptor;
        id.hardwareIds = hardwareIdsFor(m_hwid, id.vendorId, id.productId);
        QString error;
        if (!dev->create(id, &error)) return refuse(error);

        Slot s;
        s.numbered = !(b.parsed.reportIds.size() == 1 && b.parsed.reportIds.front() == 0);
        VirtualHid* raw = dev.get();
        connect(raw, &VirtualHid::request, this,
                [this, slot](VirtualHid::Request kind, int reportType, int reportId,
                             const QByteArray& data) {
                    static const char* const kNames[] = {"output", "getFeature", "setFeature"};
                    send({{QStringLiteral("type"), QStringLiteral("request")},
                          {QStringLiteral("slot"), slot},
                          {QStringLiteral("kind"), QLatin1String(kNames[int(kind)])},
                          {QStringLiteral("reportType"), reportType},
                          {QStringLiteral("reportId"), reportId},
                          {QStringLiteral("data"), QString::fromLatin1(data.toHex(' '))}});
                });
        connect(raw, &VirtualHid::injected, this, [this, slot](double us) {
            auto it = m_slots.find(slot);
            if (it == m_slots.end()) return;
            it->second.injectUs.push_back(us);
        });
        s.device = std::move(dev);
        m_slots[slot] = std::move(s);
        std::printf("slot %d %s %04x:%04x created, descriptor %zu bytes\n", slot, qPrintable(name),
                    id.vendorId, id.productId, b.descriptor.size());
        // The nodes appear a little later; tell the page once they are there.
        QTimer::singleShot(2500, this, [this, slot, b] {
            auto it = m_slots.find(slot);
            if (it == m_slots.end()) return;
            const QString nodes = it->second.device->nodes();
            std::printf("slot %d nodes: %s\n", slot, qPrintable(nodes));
            std::fflush(stdout);
            send({{QStringLiteral("type"), QStringLiteral("attached")},
                  {QStringLiteral("slot"), slot},
                  {QStringLiteral("descriptor"), hexOf(b.descriptor)},
                  {QStringLiteral("reports"), reportsOf(b.parsed)},
                  {QStringLiteral("nodes"), nodes}});
        });
    }

    void onBinary(const QByteArray& frame)
    {
        if (frame.size() < 5) return;
        const int slot = uint8_t(frame[0]);
        const uint8_t reportId = uint8_t(frame[1]);
        const int seq = uint8_t(frame[2]) | (uint8_t(frame[3]) << 8);
        auto it = m_slots.find(slot);
        if (it == m_slots.end()) return;
        Slot& s = it->second;
        if (s.lastSeq >= 0) s.lost += ((seq - s.lastSeq - 1) & 0xFFFF);
        s.lastSeq = seq;
        ++s.received;
        QByteArray report = frame.mid(4);
        if (s.numbered) report.prepend(char(reportId));
        s.device->input(report);
    }

    void stats()
    {
        for (auto& [slot, s] : m_slots) {
            if (!s.received) continue;
            std::vector<double> v = s.injectUs;
            std::sort(v.begin(), v.end());
            const double med = v.empty() ? -1 : v[v.size() / 2];
            const double p95 = v.empty() ? -1 : v[std::min(v.size() - 1, v.size() * 95 / 100)];
            std::printf(
                "slot %d: %d reports, %d lost, write->evdev median %.0f us p95 %.0f us (%zu)\n",
                slot, s.received, s.lost, med, p95, v.size());
            std::fflush(stdout);
            send({{QStringLiteral("type"), QStringLiteral("stats")},
                  {QStringLiteral("slot"), slot},
                  {QStringLiteral("received"), s.received},
                  {QStringLiteral("lost"), s.lost},
                  {QStringLiteral("injectUs"),
                   QJsonObject{{QStringLiteral("median"), med}, {QStringLiteral("p95"), p95}}}});
            s.injectUs.clear();
        }
    }

    QWebSocketServer m_server;
    QString m_hwid;
    QWebSocket* m_page = nullptr;
    std::map<int, Slot> m_slots;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("HID passthrough prototype (plan H2)"));
    cli.addHelpOption();
    cli.addOption({QStringLiteral("port"), QStringLiteral("WebSocket port"), QStringLiteral("port"),
                   QStringLiteral("39920")});
    cli.addOption({QStringLiteral("bind"), QStringLiteral("address to listen on"),
                   QStringLiteral("address"), QStringLiteral("0.0.0.0")});
    cli.addOption({QStringLiteral("selftest"),
                   QStringLiteral("encode a hid-probe reading and exit"), QStringLiteral("file")});
    cli.addOption({QStringLiteral("create"),
                   QStringLiteral("create the devices of a hid-probe reading, no page"),
                   QStringLiteral("file")});
    cli.addOption({QStringLiteral("hold"), QStringLiteral("seconds --create keeps them"),
                   QStringLiteral("seconds"), QStringLiteral("120")});
    cli.addOption({QStringLiteral("hwid"),
                   QStringLiteral("Windows hardware ids: none, usb, ours or an id"),
                   QStringLiteral("form"), QStringLiteral("none")});
    cli.addOption({QStringLiteral("ffb"), QStringLiteral("give each device the host's PID block")});
    cli.process(app);
    g_ffb = cli.isSet(QStringLiteral("ffb"));
    if (cli.isSet(QStringLiteral("selftest")))
        return selftest(cli.value(QStringLiteral("selftest")));
    const QString hwid = cli.value(QStringLiteral("hwid"));
    if (cli.isSet(QStringLiteral("create")))
        return createFrom(cli.value(QStringLiteral("create")), hwid,
                          cli.value(QStringLiteral("hold")).toInt());

    Server server(static_cast<quint16>(cli.value(QStringLiteral("port")).toUInt()),
                  QHostAddress(cli.value(QStringLiteral("bind"))), hwid);
    return app.exec();
}
