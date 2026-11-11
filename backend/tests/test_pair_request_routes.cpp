/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/PairingCrypto.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/PhonePairing.h"
#include "server/RestRouter.h"
#include "server/routes/PairRequestRoutes.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <utility>

// The pairing routes as the server registers them, driven with requests built
// by hand: who may ask, who may answer, what each answer opens, and what each
// failure costs.

namespace {

const QString kTv = QStringLiteral("192.168.1.174");
const QString kPhone = QStringLiteral("192.168.1.20");
const QString kHostHeader = QStringLiteral("192.168.1.10:8443");

/// The raw session cookie, as HttpServer::sessionTokenFromRequest reads it.
QString cookieToken(const HttpRequest& req)
{
    const QStringList parts = req.headers.value(QStringLiteral("cookie")).split(QLatin1Char(';'));
    for (const QString& part : parts) {
        const QString p = part.trimmed();
        if (p.startsWith(QLatin1String("mw_session="))) return p.mid(11);
    }
    return {};
}

/// The server around the routes: a real AuthManager, the rest by hand.
struct Server
{
    QTemporaryDir tmp;
    AppSettings settings;
    std::unique_ptr<AuthManager> auth;
    RestRouter router;
    QByteArray hostKey =
        PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
    qint64 clock = 1'800'000'000;
    QString link = QStringLiteral("https://stream.example.test/abcdefghjkmnpqrstvwxyz0123");
    QStringList guarded;            // addresses fed to the connection guard
    QStringList located;            // addresses a session was looked up for
    QStringList placed;             // addresses whose city was asked for
    QHash<QString, QString> cities; // what the lookup knows already

    Server()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        registerPairRequestRoutes(
            router, *auth,
            {[this]() { return hostKey; }, cookieToken,
             [this](const QString& address) { guarded.append(address); },
             [this](const QString&, const QString& address) { located.append(address); },
             [this]() { return link; }, [this]() { return clock; },
             [this]() { return settings.pairRequestInternet(); },
             [this]() { return settings.sessionLocationEnabled(); },
             [this](const QString& address) {
                 placed.append(address);
                 return cities.value(address);
             },
             []() {
                 // This host's links: the house's /24, its /64, and the link-local one.
                 return QList<QPair<QHostAddress, int>>{
                     {QHostAddress(QStringLiteral("192.168.1.66")), 24},
                     {QHostAddress(QStringLiteral("198.51.100.66")), 24},
                     {QHostAddress(QStringLiteral("2001:db8:1:2::66")), 64},
                     {QHostAddress(QStringLiteral("fe80::66")), 64}};
             },
             [](const QString& address) {
                 // This machine: its own addresses on those links.
                 return address == QLatin1String("192.168.1.66") ||
                        address == QLatin1String("2001:db8:1:2::66") ||
                        address == QLatin1String("fe80::66");
             }});
    }

    /// The admin page's box, as POST /api/admin/settings applies it.
    bool setInternetRequests(bool on, bool admin = true)
    {
        HttpRequest req;
        req.isLocal = admin;
        QJsonObject reply;
        const bool taken =
            writePairRequestInternet(req, {{"pair_request_internet", on}}, settings, reply);
        if (taken) CHECK_EQ(reply.value("pair_request_internet").toBool(), on);
        return taken;
    }

    QString hostId() const { return PairingCrypto::keyId(hostKey); }

    HttpResponse send(HttpRequest req)
    {
        HttpResponse reply;
        reply.statusCode = 0;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }
};

QJsonObject json(const HttpResponse& r)
{
    return QJsonDocument::fromJson(r.body).object();
}

QString error(const HttpResponse& r)
{
    return json(r).value(QStringLiteral("error")).toString();
}

/// A request from a device on this LAN, on a socket, under a LAN address of ours.
HttpRequest fromTv(const QString& method, const QString& path, const QJsonObject& body = {})
{
    HttpRequest req;
    req.method = method;
    req.path = path;
    if (!body.isEmpty()) req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    req.clientAddress = kTv;
    req.hostTrusted = true;
    req.headers.insert(QStringLiteral("host"), kHostHeader);
    return req;
}

/// The same from the administrator's page, carrying @p token as its session.
HttpRequest fromAdmin(const QString& method, const QString& path, const QString& token = {},
                      const QJsonObject& body = {})
{
    HttpRequest req = fromTv(method, path, body);
    req.clientAddress = kPhone;
    req.isLocal = true;
    if (!token.isEmpty()) req.headers.insert(QStringLiteral("cookie"), "mw_session=" + token);
    return req;
}

QString b64(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64());
}

/// A device: its key, its nonce, what it asks with and how it claims.
struct Device
{
    QByteArray pem = PairingCrypto::generatePrivateKeyPem();
    QByteArray spki = PairingCrypto::publicKeySpkiFromPrivatePem(pem);
    QByteArray nonce = PairingCrypto::generateNonce();
    QString address = kTv;
    QString id; // the request, once asked

    QJsonObject askBody() const
    {
        return {{"public_key", b64(spki)},
                {"machine_name", "MoonlightWeb TV (MiTV)"},
                {"commit", b64(PhonePairing::commitment(nonce))}};
    }

    HttpRequest ask() const
    {
        HttpRequest req = fromTv("POST", "/api/auth/pair-request", askBody());
        req.clientAddress = address;
        return req;
    }

    HttpRequest claim(const QString& hostId) const
    {
        const QByteArray sig =
            PairingCrypto::sign(pem, PhonePairing::claimMessage(hostId, id, nonce));
        HttpRequest req = fromTv("POST", "/api/auth/pair-request/" + id + "/claim",
                                 {{"nonce", b64(nonce)}, {"sig", b64(sig)}});
        req.clientAddress = address;
        return req;
    }
};

/// Ask, then claim once so the host knows the code: what the TV does at once.
QString askAndReveal(Server& s, Device& d)
{
    const HttpResponse asked = s.send(d.ask());
    d.id = json(asked).value(QStringLiteral("request")).toString();
    s.send(d.claim(s.hostId()));
    return PhonePairing::code(s.hostKey, d.spki, d.id, d.nonce);
}

/// The administrator's answer for @p d's request, with @p code typed.
HttpRequest approval(const Device& d, const QString& code, const QString& token = {})
{
    return fromAdmin("POST", "/api/auth/pair-request/" + d.id + "/approve", token,
                     {{"code", code}});
}

/// A code that is not @p code: what a person who misread it would type.
QString otherThan(const QString& code)
{
    return code == QLatin1String("0000") ? QStringLiteral("1111") : QStringLiteral("0000");
}

/// The session token a paired answer sets.
QString cookieOf(const HttpResponse& r)
{
    const QString header = r.headers.value(QStringLiteral("Set-Cookie"));
    if (!header.startsWith(QLatin1String("mw_session="))) return {};
    return header.mid(11, header.indexOf(QLatin1Char(';')) - 11);
}

void testOnlyFromThisLan()
{
    SECTION("pair-request routes: the admin's box unticked, a device asks from this LAN only");
    Server s;
    Device d;
    // A LAN device on a socket, under our address: asked, with the page to open.
    const HttpResponse ok = s.send(d.ask());
    CHECK_EQ(ok.statusCode, 200);
    CHECK(PhonePairing::isRequestId(json(ok).value("request").toString()));
    CHECK_EQ(json(ok).value("host_id").toString(), s.hostId());
    CHECK_EQ(json(ok).value("rendezvous_url").toString(), s.link);

    // Its line down, no page to open: no link, the device shows its PIN alone.
    s.link.clear();
    Device quiet;
    quiet.address = QStringLiteral("192.168.1.175");
    CHECK(!json(s.send(quiet.ask())).contains("rendezvous_url"));

    // From the internet, on a socket; from a global IPv6 address, a mesh VPN
    // (100.64/10), or a private network that is not one of this host's links.
    for (const char* elsewhere : {"203.0.113.5", "2001:db8::5", "100.64.1.2", "10.0.0.5"}) {
        Device far;
        far.address = QString::fromLatin1(elsewhere);
        CHECK_EQ(error(s.send(far.ask())), QStringLiteral("lan_only"));
    }

    // A LAN address under a bare host name, or a name that is not ours (rebinding).
    HttpRequest bare = d.ask();
    bare.hostTrusted = false;
    bare.headers.insert(QStringLiteral("host"), QStringLiteral("dualrtx:8443"));
    CHECK_EQ(error(s.send(bare)), QStringLiteral("lan_only"));
    HttpRequest rebound = d.ask();
    rebound.hostTrusted = false;
    CHECK_EQ(error(s.send(rebound)), QStringLiteral("lan_only"));
    // A LAN address under the public domain, or under no name at all: this host
    // is reached that way from anywhere.
    HttpRequest underDomain = d.ask();
    underDomain.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    CHECK_EQ(error(s.send(underDomain)), QStringLiteral("lan_only"));
    underDomain.headers.insert(QStringLiteral("host"), QString());
    CHECK_EQ(error(s.send(underDomain)), QStringLiteral("lan_only"));

    // Loopback under the public domain: a proxy on this machine forwarding the
    // internet. The address it names, or none at all, changes nothing.
    HttpRequest proxied = d.ask();
    proxied.clientAddress = QStringLiteral("127.0.0.1");
    proxied.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    CHECK_EQ(error(s.send(proxied)), QStringLiteral("lan_only"));
    proxied.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("192.168.1.80"));
    CHECK_EQ(error(s.send(proxied)), QStringLiteral("lan_only"));

    // Loopback under a local name, but a proxy naming an internet visitor.
    HttpRequest forwarded = d.ask();
    forwarded.clientAddress = QStringLiteral("127.0.0.1");
    forwarded.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("198.51.100.9"));
    CHECK_EQ(error(s.send(forwarded)), QStringLiteral("lan_only"));

    // This machine itself is not the house, under its own name or by its own
    // address on this LAN: its administrator lets a device in as such.
    HttpRequest local = d.ask();
    local.clientAddress = QStringLiteral("127.0.0.1");
    local.headers.insert(QStringLiteral("host"), QStringLiteral("localhost:8443"));
    CHECK_EQ(error(s.send(local)), QStringLiteral("lan_only"));
    Device own;
    own.address = QStringLiteral("192.168.1.66");
    CHECK_EQ(error(s.send(own.ask())), QStringLiteral("lan_only"));
    HttpRequest unnamed = Device().ask();
    unnamed.clientAddress = QStringLiteral("127.0.0.1");
    unnamed.headers.insert(QStringLiteral("host"), QString());
    CHECK_EQ(error(s.send(unnamed)), QStringLiteral("lan_only"));

    // A LAN peer passing a request along, whatever its header says: its sender
    // may stand anywhere.
    for (const char* header :
         {"x-forwarded-for", "forwarded", "cf-connecting-ip", "via", "x-real-ip"}) {
        HttpRequest relayed = Device().ask();
        relayed.headers.insert(QString::fromLatin1(header), QStringLiteral("192.168.1.80"));
        CHECK_EQ(error(s.send(relayed)), QStringLiteral("lan_only"));
    }

    // A proxy on this machine, under a local name: its sender may stand
    // anywhere too, whatever address the proxy names, or none.
    for (const char* header : {"via", "x-real-ip", "x-forwarded-host", "x-forwarded-for"}) {
        HttpRequest viaProxy = Device().ask();
        viaProxy.clientAddress = QStringLiteral("127.0.0.1");
        viaProxy.headers.insert(QStringLiteral("host"), QStringLiteral("localhost:8443"));
        viaProxy.headers.insert(QString::fromLatin1(header), QStringLiteral("192.168.1.80"));
        CHECK_EQ(error(s.send(viaProxy)), QStringLiteral("lan_only"));
    }

    // On a line: the address ICE settled on, and the request for the line's key.
    HttpRequest line = d.ask();
    line.viaTunnel = true;
    line.hostTrusted = false;
    line.tunnelKey = d.spki;
    CHECK_EQ(s.send(line).statusCode, 200);
    line.clientAddress = QStringLiteral("203.0.113.7");
    CHECK_EQ(error(s.send(line)), QStringLiteral("lan_only"));
    line.clientAddress = QStringLiteral("peer-4f2a");
    CHECK_EQ(error(s.send(line)), QStringLiteral("lan_only"));
    line.clientAddress = kTv;
    line.tunnelKey = Device().spki;
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_the_line_key"));

    // Nothing above fed the guard, nor the counter: a device that may not ask
    // is not an attacker.
    CHECK(s.guarded.isEmpty());
    CHECK_EQ(s.auth->failedAttemptCount(QStringLiteral("203.0.113.5")), 0);
}

void testFromElsewhereWhenTheAdministratorAllowsIt()
{
    SECTION("pair-request routes: from outside the LAN, while the admin's box is ticked");
    Server s;
    CHECK(!s.settings.pairRequestInternet());

    // The box is the administrator's: anyone else changes nothing, and a body
    // without its key changes nothing either.
    CHECK(!s.setInternetRequests(true, /*admin=*/false));
    CHECK(!s.settings.pairRequestInternet());
    {
        HttpRequest admin;
        admin.isLocal = true;
        QJsonObject reply;
        CHECK(!writePairRequestInternet(admin, {{"cert_auth_enabled", true}}, s.settings, reply));
        CHECK(reply.isEmpty());
        CHECK(!s.settings.pairRequestInternet());
    }
    CHECK(s.setInternetRequests(true));
    CHECK(s.settings.pairRequestInternet());

    // Ticked: every caller from elsewhere asks, and gets the page to open.
    for (const char* elsewhere : {"203.0.113.5", "2001:db8::5", "100.64.1.2", "10.0.0.5"}) {
        Device far;
        far.address = QString::fromLatin1(elsewhere);
        const HttpResponse asked = s.send(far.ask());
        CHECK_EQ(asked.statusCode, 200);
        CHECK_EQ(json(asked).value("rendezvous_url").toString(), s.link);
    }
    Device d;
    HttpRequest bare = d.ask();
    bare.hostTrusted = false;
    bare.headers.insert(QStringLiteral("host"), QStringLiteral("dualrtx:8443"));
    CHECK_EQ(s.send(bare).statusCode, 200);
    HttpRequest proxied = d.ask();
    proxied.clientAddress = QStringLiteral("127.0.0.1");
    proxied.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    proxied.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("198.51.100.9"));
    CHECK_EQ(s.send(proxied).statusCode, 200);
    Device lineTv;
    HttpRequest line = lineTv.ask();
    line.viaTunnel = true;
    line.hostTrusted = false;
    line.tunnelKey = lineTv.spki;
    line.clientAddress = QStringLiteral("203.0.113.7");
    CHECK_EQ(s.send(line).statusCode, 200);
    // ...and on a line, the request is still for the line's key.
    line.tunnelKey = Device().spki;
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_the_line_key"));

    // Everything else as on the LAN: the administrator alone, with the code the
    // device shows; then the session, bound to its key.
    Device tv;
    tv.address = QStringLiteral("198.51.100.20");
    const QString code = askAndReveal(s, tv);
    HttpRequest member = approval(tv, code, s.auth->createSession(kPhone, "Laptop"));
    member.isLocal = false;
    CHECK_EQ(error(s.send(member)), QStringLiteral("not_allowed"));
    CHECK_EQ(error(s.send(approval(tv, otherThan(code)))), QStringLiteral("code_mismatch"));
    CHECK_EQ(s.send(approval(tv, code)).statusCode, 200);
    const HttpResponse paired = s.send(tv.claim(s.hostId()));
    CHECK_EQ(json(paired).value("state").toString(), QStringLiteral("paired"));
    CHECK_EQ(s.auth->sessionPairingKey(cookieOf(paired)), tv.spki);

    // Unticked again: a request from elsewhere can no longer be claimed, and
    // nothing counts against the device, which goes back to its PIN. One of
    // the LAN goes on as before.
    Device late;
    late.address = QStringLiteral("198.51.100.21");
    askAndReveal(s, late);
    Device home;
    home.address = QStringLiteral("192.168.1.180");
    askAndReveal(s, home);
    CHECK(s.setInternetRequests(false));
    CHECK(!s.settings.pairRequestInternet());
    CHECK_EQ(error(s.send(late.claim(s.hostId()))), QStringLiteral("lan_only"));
    CHECK_EQ(error(s.send(late.ask())), QStringLiteral("lan_only"));
    CHECK_EQ(s.auth->failedAttemptCount(late.address), 0);
    CHECK_EQ(json(s.send(home.claim(s.hostId()))).value("state").toString(),
             QStringLiteral("waiting"));
}

void testPlacesKeptForTheLanAndDroppedOnceUnticked()
{
    SECTION("pair-request routes: places kept for the LAN; unticked, requests from elsewhere go");
    Server s;
    CHECK(s.setInternetRequests(true));
    QList<Device> far;
    for (int i = 0; i < PhonePairing::MAX_FROM_ELSEWHERE; ++i) {
        Device x;
        x.address = QStringLiteral("198.51.100.%1").arg(i + 1);
        const HttpResponse asked = s.send(x.ask());
        CHECK_EQ(asked.statusCode, 200);
        x.id = json(asked).value("request").toString();
        far.append(x);
    }
    // One more from elsewhere: busy, as when the book is full.
    Device extra;
    extra.address = QStringLiteral("198.51.100.200");
    const HttpResponse full = s.send(extra.ask());
    CHECK_EQ(full.statusCode, 429);
    CHECK_EQ(error(full), QStringLiteral("busy"));
    // This LAN's devices still find room.
    Device home;
    home.address = QStringLiteral("192.168.1.181");
    const HttpResponse lan = s.send(home.ask());
    CHECK_EQ(lan.statusCode, 200);
    home.id = json(lan).value("request").toString();

    // Unticked: the requests from elsewhere are gone, and their approval with
    // them. This LAN's stays, and the places are free again once ticked anew.
    CHECK(s.setInternetRequests(false));
    const QString farPath = "/api/auth/pair-request/" + far.first().id;
    CHECK_EQ(error(s.send(fromAdmin("GET", farPath))), QStringLiteral("unknown_request"));
    CHECK_EQ(error(s.send(approval(far.first(), QStringLiteral("0000")))),
             QStringLiteral("unknown_request"));
    CHECK_EQ(s.send(fromAdmin("GET", "/api/auth/pair-request/" + home.id)).statusCode, 200);
    CHECK(s.setInternetRequests(true));
    CHECK_EQ(s.send(extra.ask()).statusCode, 200);
}

void testAClaimFromElsewhereLeavesALanRequestAlone()
{
    SECTION("pair-request routes: unticked, a claim from elsewhere leaves a LAN request intact");
    Server s;
    CHECK(s.setInternetRequests(true));
    Device home;
    askAndReveal(s, home);
    CHECK(s.setInternetRequests(false));
    // The same request and its key's own claim, sent from outside the LAN.
    Device far = home;
    far.address = QStringLiteral("198.51.100.30");
    CHECK_EQ(error(s.send(far.claim(s.hostId()))), QStringLiteral("lan_only"));
    CHECK_EQ(s.auth->failedAttemptCount(far.address), 0);
    CHECK(s.guarded.isEmpty());
    // ...and the request is still there, for the device on the LAN.
    CHECK_EQ(json(s.send(home.claim(s.hostId()))).value("state").toString(),
             QStringLiteral("waiting"));
    CHECK_EQ(s.send(fromAdmin("GET", "/api/auth/pair-request/" + home.id)).statusCode, 200);
}

void testNoAdminLetsNoOneInFromElsewhere()
{
    SECTION("pair-request routes: under --noadmin, a ticked box lets no one ask from elsewhere");
    Server s;
    CHECK(s.setInternetRequests(true));
    // The server refuses every write before these routes under --noadmin; the
    // routes hold the same line on their own.
    Device far;
    far.address = QStringLiteral("198.51.100.40");
    HttpRequest asked = far.ask();
    asked.adminLocked = true;
    CHECK_EQ(error(s.send(asked)), QStringLiteral("lan_only"));
    Device earlier;
    earlier.address = QStringLiteral("198.51.100.41");
    askAndReveal(s, earlier);
    HttpRequest claim = earlier.claim(s.hostId());
    claim.adminLocked = true;
    CHECK_EQ(error(s.send(claim)), QStringLiteral("lan_only"));
    // From the LAN, the routes' own rule is unchanged.
    Device home;
    HttpRequest lan = home.ask();
    lan.adminLocked = true;
    CHECK_EQ(s.send(lan).statusCode, 200);
}

void testTheRequestSaysWhereItCameFrom()
{
    SECTION("pair-request routes: a request keeps where it came from, for the page");
    Server s;
    CHECK(s.setInternetRequests(true));
    Device home;
    askAndReveal(s, home);
    const QJsonObject lan = json(s.send(fromAdmin("GET", "/api/auth/pair-request/" + home.id)));
    CHECK_EQ(lan.value("origin").toString(), QStringLiteral("lan"));
    CHECK_EQ(lan.value("address").toString(), kTv);

    // From elsewhere: the address without its IPv4-mapped prefix...
    Device far;
    far.address = QStringLiteral("::ffff:198.51.100.20");
    askAndReveal(s, far);
    const QJsonObject mapped = json(s.send(fromAdmin("GET", "/api/auth/pair-request/" + far.id)));
    CHECK_EQ(mapped.value("origin").toString(), QStringLiteral("internet"));
    CHECK_EQ(mapped.value("address").toString(), QStringLiteral("198.51.100.20"));

    // ...and the visitor a proxy on this machine names, without its port.
    Device proxied;
    HttpRequest asked = proxied.ask();
    asked.clientAddress = QStringLiteral("127.0.0.1");
    asked.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    asked.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("203.0.113.50:4711"));
    proxied.id = json(s.send(asked)).value("request").toString();
    const QJsonObject viaProxy =
        json(s.send(fromAdmin("GET", "/api/auth/pair-request/" + proxied.id)));
    CHECK_EQ(viaProxy.value("origin").toString(), QStringLiteral("internet"));
    CHECK_EQ(viaProxy.value("address").toString(), QStringLiteral("203.0.113.50"));
}

void testTheCityOnlyWhileLocatingIsAllowed()
{
    SECTION("pair-request routes: the city of a request from elsewhere, while locating is allowed");
    Server s;
    CHECK(s.setInternetRequests(true));
    s.cities.insert(QStringLiteral("198.51.100.20"), QStringLiteral("Lyon"));
    Device home;
    askAndReveal(s, home);
    Device far;
    far.address = QStringLiteral("::ffff:198.51.100.20");
    askAndReveal(s, far);
    const auto look = [&s](const Device& d) {
        return json(s.send(fromAdmin("GET", "/api/auth/pair-request/" + d.id)));
    };

    // Not allowed, as by default: no city, and no address asked about.
    CHECK(!look(far).contains("city"));
    CHECK(s.placed.isEmpty());

    // Allowed: a device's request asks about nothing, the administrator's look
    // does, for the address the page shows; a request from this LAN never.
    s.settings.setSessionLocationEnabled(true);
    Device later;
    later.address = QStringLiteral("203.0.113.9");
    askAndReveal(s, later);
    CHECK(s.placed.isEmpty());
    CHECK_EQ(look(far).value("city").toString(), QStringLiteral("Lyon"));
    CHECK_EQ(s.placed, QStringList{QStringLiteral("198.51.100.20")});
    CHECK(!look(home).contains("city"));
    CHECK_EQ(s.placed.size(), 1);

    // Not known yet: no city, until a later look finds it.
    CHECK(!look(later).contains("city"));
    s.cities.insert(QStringLiteral("203.0.113.9"), QStringLiteral("Paris"));
    CHECK_EQ(look(later).value("city").toString(), QStringLiteral("Paris"));

    // Allowed no more: no city, known or not, and nothing asked.
    s.settings.setSessionLocationEnabled(false);
    const qsizetype asked = s.placed.size();
    CHECK(!look(far).contains("city"));
    CHECK_EQ(s.placed.size(), asked);
}

void testEachLineCallerCountsOnItsOwn()
{
    SECTION("pair-request routes: two lines with no ICE pair to name them are two callers");
    Server s;
    CHECK(s.setInternetRequests(true));
    // What ControlTunnel names a caller whose address it has not learned yet.
    const QString lineA = QStringLiteral("tunnel:aaaaaaaaaaaaaaaa");
    const QString lineB = QStringLiteral("tunnel:bbbbbbbbbbbbbbbb");
    const auto onLine = [](HttpRequest req, const Device& d) {
        req.viaTunnel = true;
        req.hostTrusted = false;
        req.tunnelKey = d.spki;
        return req;
    };
    const auto ask = [&](Device& d) {
        const HttpResponse r = s.send(onLine(d.ask(), d));
        if (r.statusCode == 200) d.id = json(r).value("request").toString();
        return r;
    };

    // Each line holds its own few: the fourth on one pushes that line's oldest
    // out, and leaves the other line's request where it is.
    QList<Device> onA;
    for (int i = 0; i < PhonePairing::MAX_PER_ADDRESS + 1; ++i) {
        Device x;
        x.address = lineA;
        CHECK_EQ(ask(x).statusCode, 200);
        onA.append(x);
        if (i == 0) {
            Device b;
            b.address = lineB;
            CHECK_EQ(ask(b).statusCode, 200);
            onA.append(b); // kept apart below
        }
    }
    const Device firstA = onA.at(0);
    const Device firstB = onA.at(1);
    CHECK_EQ(error(s.send(fromAdmin("GET", "/api/auth/pair-request/" + firstA.id))),
             QStringLiteral("unknown_request"));
    CHECK_EQ(s.send(fromAdmin("GET", "/api/auth/pair-request/" + firstB.id)).statusCode, 200);
    CHECK_EQ(s.auth->failedAttemptCount(lineA), 1);
    CHECK_EQ(s.auth->failedAttemptCount(lineB), 0);

    // Claims the line's device did not sign, on line A: A is held back, B is not.
    Device forger = onA.at(2);
    forger.pem = PairingCrypto::generatePrivateKeyPem();
    for (int i = 0; i < 2; ++i)
        CHECK_EQ(error(s.send(onLine(forger.claim(s.hostId()), forger))),
                 QStringLiteral("bad_proof"));
    CHECK(s.auth->isRateLimited(lineA));
    CHECK(!s.auth->isRateLimited(lineB));
    Device lateA;
    lateA.address = lineA;
    CHECK_EQ(error(ask(lateA)), QStringLiteral("rate_limited"));
    Device lateB;
    lateB.address = lineB;
    CHECK_EQ(ask(lateB).statusCode, 200);
    CHECK_EQ(json(s.send(onLine(firstB.claim(s.hostId()), firstB))).value("state").toString(),
             QStringLiteral("waiting"));
    // The PIN counts each line the same way: A's PIN is held back too, B's is not.
    CHECK(s.auth->validatePin(lineA, QStringLiteral("123456")).result == AuthManager::RateLimited);
    CHECK_EQ(s.auth->failedAttemptCount(lineB), 0);
}

void testOnlyTheAdministratorAnswers()
{
    SECTION("pair-request routes: no key of its own, no administrator's right: no answer");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);
    const QString path = "/api/auth/pair-request/" + d.id;

    // No session at all: 401, counted.
    CHECK_EQ(s.send(fromTv("GET", path)).statusCode, 401);
    CHECK_EQ(s.guarded.size(), 1);
    // A session, remembered or not, without the administrator's right nor a key
    // of its own to sign with: 403, even on this LAN.
    const QString member = s.auth->createSession(kPhone, "Laptop");
    const QString guest = s.auth->createSession(kPhone, "Kiosk", false, /*ephemeral=*/true);
    for (const QString& token : {member, guest}) {
        HttpRequest get = fromTv("GET", path);
        get.headers.insert(QStringLiteral("cookie"), "mw_session=" + token);
        CHECK_EQ(error(s.send(get)), QStringLiteral("not_allowed"));
        HttpRequest yes = approval(d, code, token);
        yes.isLocal = false;
        CHECK_EQ(error(s.send(yes)), QStringLiteral("not_allowed"));
        HttpRequest no = fromTv("POST", path + "/refuse");
        no.headers.insert(QStringLiteral("cookie"), "mw_session=" + token);
        CHECK_EQ(error(s.send(no)), QStringLiteral("not_allowed"));
    }
    CHECK_EQ(s.guarded.size(), 1);

    // The administrator: the device's name, whether its code can be typed, and
    // never the code itself.
    const HttpResponse seen = s.send(fromAdmin("GET", path));
    CHECK_EQ(seen.statusCode, 200);
    CHECK_EQ(json(seen).value("machine_name").toString(), QStringLiteral("MoonlightWeb TV (MiTV)"));
    CHECK_EQ(json(seen).value("state").toString(), QStringLiteral("waiting"));
    CHECK(json(seen).value("ready").toBool());
    CHECK(!json(seen).contains("code"));
    CHECK_EQ(error(s.send(fromAdmin("GET", "/api/auth/pair-request/short"))),
             QStringLiteral("unknown_request"));
}

void testAPairedSessionOnThisLanAnswersToo()
{
    SECTION("pair-request routes: from this LAN, a paired session with its key answers too");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);
    const QString path = "/api/auth/pair-request/" + d.id;

    // A phone of the house, paired by its PIN, its key bound: not the administrator.
    const QByteArray pem = PairingCrypto::generatePrivateKeyPem();
    const QString member = s.auth->createSession(kPhone, "Kitchen phone");
    CHECK(s.auth->bindSessionKey(member, PairingCrypto::publicKeySpkiFromPrivatePem(pem)));
    const auto asMember = [&](HttpRequest req) {
        req.isLocal = false;
        req.headers.insert(QStringLiteral("cookie"), "mw_session=" + member);
        return req;
    };
    const auto signedAnswer = [&](const QString& typed) {
        HttpRequest req = asMember(approval(d, typed));
        const QByteArray sig =
            PairingCrypto::sign(pem, PhonePairing::approveMessage(s.hostId(), d.id, typed));
        QJsonObject body = QJsonDocument::fromJson(req.body).object();
        body.insert(QStringLiteral("sig"), b64(sig));
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        return req;
    };

    // Where it stands decides, as ICE or the socket says it, never a header.
    HttpRequest line = asMember(fromTv("GET", path));
    line.viaTunnel = true;
    line.hostTrusted = false;
    line.clientAddress = QStringLiteral("203.0.113.7");
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_allowed"));
    line.clientAddress = QStringLiteral("peer-4f2a");
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_allowed"));
    HttpRequest global = asMember(fromTv("GET", path));
    global.clientAddress = QStringLiteral("2001:db8::20");
    CHECK_EQ(error(s.send(global)), QStringLiteral("not_allowed"));
    HttpRequest untrusted = asMember(fromTv("GET", path));
    untrusted.hostTrusted = false;
    CHECK_EQ(error(s.send(untrusted)), QStringLiteral("not_allowed"));
    CHECK_EQ(s.guarded.size(), 0);

    // A public IPv4 address, even inside the prefix of one of this host's
    // interfaces, is not the house.
    line.clientAddress = QStringLiteral("198.51.100.50");
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_allowed"));
    // Off this host's links, private or not: no.
    line.clientAddress = QStringLiteral("10.0.0.5");
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_allowed"));
    line.clientAddress = QStringLiteral("2001:db8:1:3::50");
    CHECK_EQ(error(s.send(line)), QStringLiteral("not_allowed"));
    HttpRequest otherLan = asMember(fromTv("GET", path));
    otherLan.clientAddress = QStringLiteral("10.0.0.5");
    CHECK_EQ(error(s.send(otherLan)), QStringLiteral("not_allowed"));
    // A public IPv6 address in this host's /64: on a line, where ICE found it,
    // yes; on a socket, no.
    line.clientAddress = QStringLiteral("2001:db8:1:2::50");
    CHECK_EQ(s.send(line).statusCode, 200);
    HttpRequest sameV6 = asMember(fromTv("GET", path));
    sameV6.clientAddress = QStringLiteral("2001:db8:1:2::50");
    CHECK_EQ(error(s.send(sameV6)), QStringLiteral("not_allowed"));

    // On a line that ICE put on this LAN, as the phone that scanned the QR code is.
    line.clientAddress = kPhone;
    const HttpResponse seen = s.send(line);
    CHECK_EQ(seen.statusCode, 200);
    CHECK(!json(seen).contains("code"));

    // Its answer is signed by its key, with the code typed from the device.
    CHECK_EQ(error(s.send(asMember(approval(d, code)))), QStringLiteral("bad_signature"));
    CHECK_EQ(error(s.send(signedAnswer(otherThan(code)))), QStringLiteral("code_mismatch"));
    CHECK_EQ(s.send(signedAnswer(code)).statusCode, 200);
    const HttpResponse paired = s.send(d.claim(s.hostId()));
    CHECK_EQ(json(paired).value("state").toString(), QStringLiteral("paired"));
    // The device goes with the session that let it in, as with the administrator.
    CHECK_EQ(s.auth->sessionForToken(cookieOf(paired)).approvedBy,
             s.auth->sessionIdForToken(member));

    // It may say no as well, signed as a yes is.
    Device other;
    askAndReveal(s, other);
    HttpRequest no = asMember(fromTv("POST", "/api/auth/pair-request/" + other.id + "/refuse"));
    CHECK_EQ(error(s.send(no)), QStringLiteral("bad_signature"));
    const QByteArray otherKeySig = PairingCrypto::sign(
        PairingCrypto::generatePrivateKeyPem(), PhonePairing::refuseMessage(s.hostId(), other.id));
    no.body = QJsonDocument(QJsonObject{{"sig", b64(otherKeySig)}}).toJson(QJsonDocument::Compact);
    CHECK_EQ(error(s.send(no)), QStringLiteral("bad_signature"));
    const QByteArray noSig =
        PairingCrypto::sign(pem, PhonePairing::refuseMessage(s.hostId(), other.id));
    no.body = QJsonDocument(QJsonObject{{"sig", b64(noSig)}}).toJson(QJsonDocument::Compact);
    CHECK_EQ(s.send(no).statusCode, 200);

    // A session with no key of its own, even on a line ICE put on this LAN: no.
    const QString keyless = s.auth->createSession(kPhone, "Old laptop");
    HttpRequest bare = fromTv("GET", path);
    bare.viaTunnel = true;
    bare.hostTrusted = false;
    bare.clientAddress = kPhone;
    bare.headers.insert(QStringLiteral("cookie"), "mw_session=" + keyless);
    CHECK_EQ(error(s.send(bare)), QStringLiteral("not_allowed"));
}

/// A member of the house, its key bound, for the sections below.
struct Member
{
    QByteArray pem = PairingCrypto::generatePrivateKeyPem();
    QString token;

    Member(Server& s, const QString& name, bool ephemeral = false)
    {
        token = s.auth->createSession(kPhone, name, false, ephemeral);
        CHECK(s.auth->bindSessionKey(token, PairingCrypto::publicKeySpkiFromPrivatePem(pem)));
    }

    HttpRequest get(const QString& id) const
    {
        HttpRequest req = fromTv("GET", "/api/auth/pair-request/" + id);
        req.headers.insert(QStringLiteral("cookie"), "mw_session=" + token);
        return req;
    }

    HttpRequest yes(Server& s, const Device& d, const QString& code) const
    {
        HttpRequest req = approval(d, code);
        req.isLocal = false;
        req.headers.insert(QStringLiteral("cookie"), "mw_session=" + token);
        const QByteArray sig =
            PairingCrypto::sign(pem, PhonePairing::approveMessage(s.hostId(), d.id, code));
        req.body = QJsonDocument(QJsonObject{{"code", code}, {"sig", b64(sig)}})
                       .toJson(QJsonDocument::Compact);
        return req;
    }
};

void testTheHouseAnswersTheHouseOnly()
{
    SECTION("pair-request routes: a request from elsewhere stays the administrator's alone");
    Server s;
    CHECK(s.setInternetRequests(true));
    Device far;
    far.address = QStringLiteral("203.0.113.5");
    const QString code = askAndReveal(s, far);
    const Member m(s, "Kitchen phone");

    // To a member of the house, it is a request it may not see: neither its
    // name, nor its address, nor where that is.
    const HttpResponse seen = s.send(m.get(far.id));
    CHECK_EQ(error(seen), QStringLiteral("not_allowed"));
    CHECK(!json(seen).contains("address"));
    CHECK_EQ(error(s.send(m.yes(s, far, code))), QStringLiteral("not_allowed"));
    HttpRequest no = m.get(far.id);
    no.method = QStringLiteral("POST");
    no.path += QStringLiteral("/refuse");
    CHECK_EQ(error(s.send(no)), QStringLiteral("not_allowed"));

    // The administrator sees where it comes from, and answers.
    const HttpResponse admin = s.send(fromAdmin("GET", "/api/auth/pair-request/" + far.id));
    CHECK_EQ(json(admin).value("origin").toString(), QStringLiteral("internet"));
    CHECK_EQ(json(admin).value("address").toString(), QStringLiteral("203.0.113.5"));
    CHECK_EQ(s.send(approval(far, code)).statusCode, 200);
}

void testARequestOffTheHostsLinksStaysTheAdministrators()
{
    SECTION(
        "pair-request routes: a device asking from off this host's links is the administrator's");
    Server s;
    // A private address, but another network than this host's: a VPN, a
    // neighbour's subnet. It is the internet: it asks only while the box is
    // ticked, on a socket as on a line.
    Device off;
    off.address = QStringLiteral("10.0.0.5");
    CHECK_EQ(error(s.send(off.ask())), QStringLiteral("lan_only"));
    HttpRequest line = off.ask();
    line.viaTunnel = true;
    line.hostTrusted = false;
    line.tunnelKey = off.spki;
    CHECK_EQ(error(s.send(line)), QStringLiteral("lan_only"));
    CHECK(s.setInternetRequests(true));
    const QString code = askAndReveal(s, off);
    // The house does not see it.
    const Member m(s, "Kitchen phone");
    CHECK_EQ(error(s.send(m.get(off.id))), QStringLiteral("not_allowed"));
    CHECK_EQ(error(s.send(m.yes(s, off, code))), QStringLiteral("not_allowed"));
    // The administrator sees it, from the internet, and answers.
    const HttpResponse admin = s.send(fromAdmin("GET", "/api/auth/pair-request/" + off.id));
    CHECK_EQ(json(admin).value("origin").toString(), QStringLiteral("internet"));
    CHECK_EQ(s.send(approval(off, code)).statusCode, 200);
}

void testOffALineTheHouseAnswersUnderALocalName()
{
    SECTION("pair-request routes: off a line, the house answers under a local name only");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);
    const Member m(s, "Kitchen phone");

    // On a socket from this LAN, under the public domain or under no name at
    // all: where this host is reached from anywhere, not the house.
    HttpRequest seen = m.get(d.id);
    seen.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    CHECK_EQ(error(s.send(seen)), QStringLiteral("not_allowed"));
    seen.headers.insert(QStringLiteral("host"), QString());
    CHECK_EQ(error(s.send(seen)), QStringLiteral("not_allowed"));
    HttpRequest underDomain = m.yes(s, d, code);
    underDomain.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    CHECK_EQ(error(s.send(underDomain)), QStringLiteral("not_allowed"));

    // Under an mDNS name, or this host's private address: the house.
    seen.headers.insert(QStringLiteral("host"), QStringLiteral("dualrtx.local:8443"));
    CHECK_EQ(s.send(seen).statusCode, 200);

    // Passed along by another machine on this LAN: not the house, whatever
    // the proxy's header says.
    for (const char* header :
         {"x-forwarded-for", "forwarded", "cf-connecting-ip", "via", "x-real-ip"}) {
        HttpRequest relayed = m.yes(s, d, code);
        relayed.headers.insert(QString::fromLatin1(header), QStringLiteral("1.1 relay"));
        CHECK_EQ(error(s.send(relayed)), QStringLiteral("not_allowed"));
    }

    // From this machine itself, by its own address on this LAN or the
    // loopback, on a socket or on a line: never the house.
    for (const char* own : {"192.168.1.66", "127.0.0.1"}) {
        HttpRequest mine = m.yes(s, d, code);
        mine.clientAddress = QString::fromLatin1(own);
        mine.headers.insert(QStringLiteral("host"), QStringLiteral("dualrtx.local:8443"));
        CHECK_EQ(error(s.send(mine)), QStringLiteral("not_allowed"));
        mine.viaTunnel = true;
        CHECK_EQ(error(s.send(mine)), QStringLiteral("not_allowed"));
    }

    // On a line, the public domain changes nothing: ICE says where it stands.
    HttpRequest line = m.yes(s, d, code);
    line.viaTunnel = true;
    line.hostTrusted = false;
    line.headers.insert(QStringLiteral("host"), QStringLiteral("pc.example.test"));
    CHECK_EQ(s.send(line).statusCode, 200);
    const HttpResponse paired = s.send(d.claim(s.hostId()));
    CHECK_EQ(json(paired).value("state").toString(), QStringLiteral("paired"));
}

void testATemporarySessionLetsNoDeviceIn()
{
    SECTION("pair-request routes: a session kept without \"remember me\" lets no device in");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);

    // A member of the house, signed in for this visit only: it sees, it may
    // say no, but not yes.
    const Member visit(s, "Visitor's phone", /*ephemeral=*/true);
    CHECK_EQ(s.send(visit.get(d.id)).statusCode, 200);
    CHECK_EQ(error(s.send(visit.yes(s, d, code))), QStringLiteral("temporary_session"));

    // The administrator's own temporary session neither.
    HttpRequest admin = approval(d, code, visit.token);
    CHECK_EQ(error(s.send(admin)), QStringLiteral("temporary_session"));

    // The machine itself, with no session at all, still does.
    CHECK_EQ(s.send(approval(d, code)).statusCode, 200);
    CHECK_EQ(json(s.send(d.claim(s.hostId()))).value("state").toString(), QStringLiteral("paired"));
}

void testAChainOfApproversGoesAsOne()
{
    SECTION("pair-request routes: a device let in by a device goes with the first one");
    Server s;
    const Member m(s, "Kitchen phone");

    // The phone lets the first TV in; that TV, a paired session of the house
    // with its own key, lets the second one in.
    Device first;
    const QString firstCode = askAndReveal(s, first);
    CHECK_EQ(s.send(m.yes(s, first, firstCode)).statusCode, 200);
    const QString firstTv = cookieOf(s.send(first.claim(s.hostId())));
    CHECK(s.auth->validateSession(firstTv));

    Device second;
    second.address = QStringLiteral("192.168.1.177");
    const QString secondCode = askAndReveal(s, second);
    HttpRequest yes = approval(second, secondCode);
    yes.isLocal = false;
    yes.clientAddress = kTv;
    yes.headers.insert(QStringLiteral("cookie"), "mw_session=" + firstTv);
    const QByteArray sig = PairingCrypto::sign(
        first.pem, PhonePairing::approveMessage(s.hostId(), second.id, secondCode));
    yes.body = QJsonDocument(QJsonObject{{"code", secondCode}, {"sig", b64(sig)}})
                   .toJson(QJsonDocument::Compact);
    CHECK_EQ(s.send(yes).statusCode, 200);
    const QString secondTv = cookieOf(s.send(second.claim(s.hostId())));
    CHECK(s.auth->validateSession(secondTv));
    CHECK_EQ(s.auth->sessionForToken(secondTv).approvedBy, s.auth->sessionIdForToken(firstTv));

    // The phone revoked: both TVs go with it.
    s.auth->destroySession(s.auth->sessionIdForToken(m.token));
    CHECK(!s.auth->validateSession(firstTv));
    CHECK(!s.auth->validateSession(secondTv));
}

void testADeviceOutlivesAnApproverThatLoggedOut()
{
    SECTION("pair-request routes: the approver logs out, the device stays");
    Server s;
    const Member m(s, "Kitchen phone");
    Device d;
    const QString code = askAndReveal(s, d);
    CHECK_EQ(s.send(m.yes(s, d, code)).statusCode, 200);
    const QString tv = cookieOf(s.send(d.claim(s.hostId())));
    CHECK(s.auth->validateSession(tv));

    // The phone leaves by itself: the TV stays, now the administrator's.
    CHECK(s.auth->logoutSession(m.token));
    CHECK(!s.auth->validateSession(m.token));
    CHECK(s.auth->validateSession(tv));
    CHECK(s.auth->sessionForToken(tv).approvedBy.isEmpty());

    // A revocation from the list of sessions still takes what a session let in.
    const Member other(s, "Hall phone");
    Device next;
    const QString nextCode = askAndReveal(s, next);
    CHECK_EQ(s.send(other.yes(s, next, nextCode)).statusCode, 200);
    const QString nextTv = cookieOf(s.send(next.claim(s.hostId())));
    CHECK(s.auth->validateSession(nextTv));
    s.auth->destroySession(s.auth->sessionIdForToken(other.token));
    CHECK(!s.auth->validateSession(nextTv));
    CHECK(s.auth->validateSession(tv));
}

void testADeviceOutlivesAnApproverThatRanOut()
{
    SECTION("pair-request routes: the approver's time runs out, the device stays");
    Server s;
    const Member m(s, "Kitchen phone");
    Device d;
    const QString code = askAndReveal(s, d);
    CHECK_EQ(s.send(m.yes(s, d, code)).statusCode, 200);
    const QString tv = cookieOf(s.send(d.claim(s.hostId())));
    CHECK(s.auth->validateSession(tv));

    // The phone's session last seen long ago, as the server finds it after a
    // restart: dropped at load, and the TV kept, now the administrator's.
    s.auth->saveSessions();
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/sessions.json";
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QJsonArray sessions = root.value("sessions").toArray();
    file.close();
    const QString phoneId = s.auth->sessionIdForToken(m.token);
    for (qsizetype i = 0; i < sessions.size(); ++i) {
        QJsonObject o = sessions.at(i).toObject();
        if (o.value("token").toString() != phoneId) continue;
        o["created_at"] = 1'000'000'000;
        o["last_seen"] = 1'000'000'000;
        sessions[i] = o;
    }
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    root["sessions"] = sessions;
    file.write(QJsonDocument(root).toJson());
    file.close();
    s.auth->loadSessions();
    CHECK(!s.auth->validateSession(m.token));
    CHECK(s.auth->validateSession(tv));
    CHECK(s.auth->sessionForToken(tv).approvedBy.isEmpty());
}

void testAPlayerCookieIsNoSession()
{
    SECTION("pair-request routes: a guest's player cookie answers nothing");
    Server s;
    Device d;
    askAndReveal(s, d);
    HttpRequest get = fromTv("GET", "/api/auth/pair-request/" + d.id);
    get.headers.insert(QStringLiteral("cookie"), QStringLiteral("mw_player=guest-cookie"));
    CHECK_EQ(s.send(get).statusCode, 401);
    CHECK_EQ(s.guarded.size(), 1);
}

void testTheCodeIsTypedAndSigned()
{
    SECTION("pair-request routes: the code typed from the device, signed by the session");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);

    // A session with a pairing key signs what it sends; another key's signature
    // or none is refused, and counted.
    const QByteArray pagePem = PairingCrypto::generatePrivateKeyPem();
    const QString admin = s.auth->createSession(kPhone, "Owner's phone");
    CHECK(s.auth->bindSessionKey(admin, PairingCrypto::publicKeySpkiFromPrivatePem(pagePem)));
    CHECK_EQ(error(s.send(approval(d, code, admin))), QStringLiteral("bad_signature"));
    const auto signedBy = [&](const QByteArray& pem, const QString& typed) {
        HttpRequest req = approval(d, typed, admin);
        const QByteArray sig =
            PairingCrypto::sign(pem, PhonePairing::approveMessage(s.hostId(), d.id, typed));
        QJsonObject body = QJsonDocument::fromJson(req.body).object();
        body.insert(QStringLiteral("sig"), b64(sig));
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        return req;
    };
    CHECK_EQ(error(s.send(signedBy(PairingCrypto::generatePrivateKeyPem(), code))),
             QStringLiteral("bad_signature"));
    CHECK_EQ(s.guarded.size(), 2);

    // Signed, but not the device's code: tries left, then refused.
    const HttpResponse wrong = s.send(signedBy(pagePem, otherThan(code)));
    CHECK_EQ(wrong.statusCode, 409);
    CHECK_EQ(error(wrong), QStringLiteral("code_mismatch"));
    CHECK_EQ(json(wrong).value("tries_left").toInt(), PhonePairing::CODE_TRIES - 1);

    // Signed, the code as a person types it: approved, by that session.
    const HttpResponse yes = s.send(signedBy(pagePem, code.toLower()));
    CHECK_EQ(yes.statusCode, 200);
    const HttpResponse paired = s.send(d.claim(s.hostId()));
    CHECK_EQ(json(paired).value("state").toString(), QStringLiteral("paired"));
    const QString tv = cookieOf(paired);
    CHECK(s.auth->validateSession(tv));
    CHECK_EQ(s.auth->sessionForToken(tv).approvedBy, s.auth->sessionIdForToken(admin));
}

void testThisMachineNeedsNoSignature()
{
    SECTION("pair-request routes: the host machine approves with no session to sign with");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);
    CHECK_EQ(s.send(approval(d, code)).statusCode, 200);
    const HttpResponse paired = s.send(d.claim(s.hostId()));
    CHECK_EQ(paired.statusCode, 200);
    const QString tv = cookieOf(paired);
    // The session a PIN would have opened: remembered, bound to the device's key,
    // named after it, looked up where it is.
    CHECK(paired.headers.value(QStringLiteral("Set-Cookie")).contains("Max-Age=7776000"));
    CHECK_EQ(json(paired).value("pairing_key").toString(), QStringLiteral("bound"));
    CHECK_EQ(json(paired).value("host_id").toString(), s.hostId());
    CHECK_EQ(s.auth->sessionPairingKey(tv), d.spki);
    CHECK_EQ(s.auth->sessionForToken(tv).machineName, QStringLiteral("MoonlightWeb TV (MiTV)"));
    CHECK(s.auth->sessionForToken(tv).approvedBy.isEmpty());
    CHECK_EQ(s.located, QStringList{kTv});
}

void testALostAnswerIsAskedAgain()
{
    SECTION("pair-request routes: a paired answer lost on the way, and the claim again");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);
    const QString admin = s.auth->createSession(kPhone, "Owner's phone");
    CHECK_EQ(s.send(approval(d, code, admin)).statusCode, 200);
    const QString first = cookieOf(s.send(d.claim(s.hostId())));
    CHECK(s.auth->validateSession(first));

    // The same claim a moment later: a new session in place of the first.
    s.clock += 5;
    const HttpResponse again = s.send(d.claim(s.hostId()));
    CHECK_EQ(again.statusCode, 200);
    const QString second = cookieOf(again);
    CHECK(!second.isEmpty() && second != first);
    CHECK(!s.auth->validateSession(first));
    CHECK(s.auth->validateSession(second));
    CHECK_EQ(s.auth->sessionForToken(second).approvedBy, s.auth->sessionIdForToken(admin));

    // Revoked in the meantime: the claim again is refused, nothing reopens.
    s.auth->destroySession(s.auth->sessionIdForToken(second));
    CHECK_EQ(error(s.send(d.claim(s.hostId()))), QStringLiteral("refused"));
    CHECK_EQ(error(s.send(d.claim(s.hostId()))), QStringLiteral("unknown_request"));

    // A minute after pairing, the request is gone.
    Device late;
    late.address = QStringLiteral("192.168.1.176");
    const QString lateCode = askAndReveal(s, late);
    CHECK_EQ(s.send(approval(late, lateCode)).statusCode, 200);
    CHECK_EQ(s.send(late.claim(s.hostId())).statusCode, 200);
    s.clock += PhonePairing::GRACE_SECS + 1;
    CHECK_EQ(error(s.send(late.claim(s.hostId()))), QStringLiteral("unknown_request"));
    // None of that was a failure of the device's.
    CHECK_EQ(s.auth->failedAttemptCount(kTv), 0);
    CHECK_EQ(s.auth->failedAttemptCount(late.address), 0);
}

void testRevokingTheApproverRevokesTheDevice()
{
    SECTION("pair-request routes: the device goes with the session that approved it");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);
    const QString admin = s.auth->createSession(kPhone, "Owner's phone");
    CHECK_EQ(s.send(approval(d, code, admin)).statusCode, 200);
    const QString tv = cookieOf(s.send(d.claim(s.hostId())));
    CHECK(s.auth->validateSession(tv));
    s.auth->destroySession(s.auth->sessionIdForToken(admin));
    CHECK(!s.auth->validateSession(tv));

    // Revoked between its yes and the device's claim: nothing is made.
    Device next;
    next.address = QStringLiteral("192.168.1.177");
    const QString nextCode = askAndReveal(s, next);
    const QString other = s.auth->createSession(kPhone, "Owner's tablet");
    CHECK_EQ(s.send(approval(next, nextCode, other)).statusCode, 200);
    s.auth->destroySession(s.auth->sessionIdForToken(other));
    CHECK_EQ(error(s.send(next.claim(s.hostId()))), QStringLiteral("refused"));
}

void testWhatEachFailureCosts()
{
    SECTION("pair-request routes: what each failure costs, and whom");
    Server s;
    Device d;
    const QString code = askAndReveal(s, d);

    // A request nobody knows, or no longer: the device shows a new code, and
    // nothing is counted against it.
    Device forgotten = d;
    forgotten.id = QStringLiteral("AbCdEfGhIjKlMnOp");
    for (int i = 0; i < 5; ++i)
        CHECK_EQ(error(s.send(forgotten.claim(s.hostId()))), QStringLiteral("unknown_request"));
    CHECK_EQ(s.auth->failedAttemptCount(kTv), 0);
    CHECK(s.guarded.isEmpty());

    // A claim the device's key did not sign: counted against who sent it.
    Device stranger = d;
    stranger.pem = PairingCrypto::generatePrivateKeyPem();
    stranger.address = QStringLiteral("192.168.1.65");
    CHECK_EQ(error(s.send(stranger.claim(s.hostId()))), QStringLiteral("bad_proof"));
    CHECK_EQ(s.auth->failedAttemptCount(stranger.address), 1);
    CHECK_EQ(s.guarded, QStringList{stranger.address});
    // ...and the request is still the device's.
    CHECK_EQ(json(s.send(d.claim(s.hostId()))).value("state").toString(),
             QStringLiteral("waiting"));

    // A refusal counts against the device's address, not the administrator's.
    const QString path = "/api/auth/pair-request/" + d.id;
    CHECK_EQ(s.send(fromAdmin("POST", path + "/refuse")).statusCode, 200);
    CHECK_EQ(s.auth->failedAttemptCount(kTv), 1);
    CHECK_EQ(s.auth->failedAttemptCount(kPhone), 0);
    CHECK_EQ(error(s.send(d.claim(s.hostId()))), QStringLiteral("refused"));
    CHECK_EQ(error(s.send(approval(d, code))), QStringLiteral("unknown_request"));

    // Wrong codes, too many: refused the same, and counted the same.
    Device typo;
    typo.address = QStringLiteral("192.168.1.67");
    const QString misread = otherThan(askAndReveal(s, typo));
    for (int i = 1; i < PhonePairing::CODE_TRIES; ++i)
        CHECK_EQ(error(s.send(approval(typo, misread))), QStringLiteral("code_mismatch"));
    CHECK_EQ(error(s.send(approval(typo, misread))), QStringLiteral("too_many_tries"));
    CHECK_EQ(s.auth->failedAttemptCount(typo.address), 1);
    CHECK_EQ(error(s.send(typo.claim(s.hostId()))), QStringLiteral("refused"));

    // Locked out like after wrong PINs: neither ask nor claim.
    Device locked;
    locked.address = QStringLiteral("192.168.1.68");
    askAndReveal(s, locked);
    for (int i = 0; i < 3; ++i)
        s.auth->countPairingFailure(locked.address);
    const HttpResponse asked = s.send(locked.ask());
    CHECK_EQ(asked.statusCode, 429);
    CHECK_EQ(error(asked), QStringLiteral("rate_limited"));
    CHECK(json(asked).value("lockout_seconds").toInt() > 0);
    CHECK_EQ(s.send(locked.claim(s.hostId())).statusCode, 429);
}

void testOneAddressHoldsAFew()
{
    SECTION("pair-request routes: an address asking for more than it holds pays for it");
    Server s;
    const QString crowd = QStringLiteral("192.168.1.90");
    for (int i = 0; i < PhonePairing::MAX_PER_ADDRESS; ++i) {
        Device x;
        x.address = crowd;
        CHECK_EQ(s.send(x.ask()).statusCode, 200);
    }
    CHECK_EQ(s.auth->failedAttemptCount(crowd), 0);
    for (int i = 0; i < 3; ++i) {
        Device x;
        x.address = crowd;
        CHECK_EQ(s.send(x.ask()).statusCode, 200);
    }
    CHECK_EQ(s.auth->failedAttemptCount(crowd), 3);
    Device more;
    more.address = crowd;
    CHECK_EQ(error(s.send(more.ask())), QStringLiteral("rate_limited"));
    // Another device of this LAN still asks.
    Device tv;
    CHECK_EQ(s.send(tv.ask()).statusCode, 200);
}

} // namespace

void run_pair_request_routes_tests()
{
    testOnlyFromThisLan();
    testFromElsewhereWhenTheAdministratorAllowsIt();
    testPlacesKeptForTheLanAndDroppedOnceUnticked();
    testAClaimFromElsewhereLeavesALanRequestAlone();
    testNoAdminLetsNoOneInFromElsewhere();
    testTheRequestSaysWhereItCameFrom();
    testTheCityOnlyWhileLocatingIsAllowed();
    testEachLineCallerCountsOnItsOwn();
    testOnlyTheAdministratorAnswers();
    testAPairedSessionOnThisLanAnswersToo();
    testTheHouseAnswersTheHouseOnly();
    testARequestOffTheHostsLinksStaysTheAdministrators();
    testOffALineTheHouseAnswersUnderALocalName();
    testATemporarySessionLetsNoDeviceIn();
    testAChainOfApproversGoesAsOne();
    testADeviceOutlivesAnApproverThatLoggedOut();
    testADeviceOutlivesAnApproverThatRanOut();
    testAPlayerCookieIsNoSession();
    testTheCodeIsTypedAndSigned();
    testThisMachineNeedsNoSignature();
    testALostAnswerIsAskedAgain();
    testRevokingTheApproverRevokesTheDevice();
    testWhatEachFailureCosts();
    testOneAddressHoldsAFew();
}
