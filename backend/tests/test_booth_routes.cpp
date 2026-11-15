/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A booth's stream PIN routes (issue #68), driven with requests built by hand
 * around the real ShareManager, BoothPin and OwnerProof: who may join, in which
 * order a join is refused, one seat per session, who may read the PIN, a
 * take-over held to the same steps as a join, and the doors the PIN never
 * leaves by (a URL, a log, an error body).
 */
#include "test_framework.h"

#include "common/Logger.h"
#include "common/PairingCrypto.h"
#include "server/BoothPin.h"
#include "server/OwnerProof.h"
#include "server/RestRouter.h"
#include "server/ShareManager.h"
#include "server/routes/BoothRoutes.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStringList>

#include <memory>

namespace {

const QString kHost = QStringLiteral("host-uuid-A");
const QString kHostKeyId = QStringLiteral("host-key-id");
const qint64 kNow = 1'000'000;

struct Browser
{
    QByteArray pem;
    QByteArray spki;
};

Browser newBrowser()
{
    Browser b;
    b.pem = PairingCrypto::generatePrivateKeyPem();
    b.spki = PairingCrypto::publicKeySpkiFromPrivatePem(b.pem);
    return b;
}

/// A booth around the routes, every dependency in plain sight.
struct Booth
{
    std::unique_ptr<ShareManager> share;
    BoothPin pins;
    OwnerProof proofs;
    RestRouter router;
    BoothRoutesDeps::Stream stream;
    QHash<QString, QByteArray> keys; ///< session token → its pairing key
    QStringList reported;            ///< addresses fed to ConnectionGuard
    QString level = QStringLiteral("viewer");
    BoothRoutesDeps deps;

    Booth()
    {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                      QStringLiteral("/share.json"));
        share = std::make_unique<ShareManager>();
        stream.live = true;
        stream.key = QStringLiteral("stream-key-1");
        stream.appId = 7;
        stream.owner = StreamOwner::of(false, QStringLiteral("owner-token"));

        deps.sessionToken = [](const HttpRequest& req) {
            return req.headers.value(QStringLiteral("x-session"));
        };
        deps.pairingKey = [this](const QString& token) { return keys.value(token); };
        deps.machineName = [](const QString& token) { return QStringLiteral("PC-") + token; };
        deps.hostKeyId = []() { return kHostKeyId; };
        deps.addressKey = [](const HttpRequest& req) { return req.clientAddress; };
        deps.reportAuthFailure = [this](const QString& address) { reported.append(address); };
        deps.ownerStream = [this](const QString& hostUuid, int) {
            return hostUuid == kHost ? stream : BoothRoutesDeps::Stream{};
        };
        deps.liveStreamKeys = [this]() {
            return stream.live ? QSet<QString>{stream.key} : QSet<QString>{};
        };
        deps.joinLevel = [this]() { return level; };
        deps.nowMs = []() { return kNow; };
        registerBoothRoutes(router, *share, pins, proofs, deps);
    }

    QJsonObject proof(const Browser& b, const QString& token, const QString& action,
                      const QString& host = kHost)
    {
        const QByteArray nonce = proofs.issue(token, kNow);
        const QByteArray sig =
            PairingCrypto::sign(b.pem, OwnerProof::boothMessage(kHostKeyId, nonce, action, host));
        QJsonObject p;
        p[QStringLiteral("nonce")] = QString::fromLatin1(nonce.toBase64());
        p[QStringLiteral("sig")] = QString::fromLatin1(sig.toBase64());
        return p;
    }

    HttpResponse post(const QString& path, const QJsonObject& body, const QString& token,
                      const QString& address = QStringLiteral("192.168.1.20"),
                      bool thisMachine = false, bool boothPin = true)
    {
        HttpRequest req;
        req.method = QStringLiteral("POST");
        req.path = path;
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        req.clientAddress = address;
        req.boothPin = boothPin;
        req.adminLocked = boothPin;
        req.isThisMachine = thisMachine;
        if (!token.isEmpty()) req.headers.insert(QStringLiteral("x-session"), token);
        req.headers.insert(QStringLiteral("user-agent"), QStringLiteral("Mozilla/5.0 test"));
        HttpResponse reply;
        reply.statusCode = 0;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }

    QJsonObject join(const Browser& b, const QString& token, const QString& pin,
                     int* status = nullptr, const QString& address = QStringLiteral("192.168.1.20"),
                     HttpResponse* raw = nullptr)
    {
        QJsonObject body{{QStringLiteral("host_uuid"), kHost},
                         {QStringLiteral("pin"), pin},
                         {QStringLiteral("owner_proof"), proof(b, token, "booth_join")}};
        const HttpResponse r = post(QStringLiteral("/api/booth/join"), body, token, address);
        if (status) *status = r.statusCode;
        if (raw) *raw = r;
        return QJsonDocument::fromJson(r.body).object();
    }

    QString pinNow()
    {
        if (!pins.has(stream.key)) pins.mint(stream.key);
        return pins.read(stream.key);
    }

    /// A start (or a stop) that would take this stream over, as the server
    /// asks it once the gate said its PIN is needed.
    std::optional<HttpResponse> takeOver(const QJsonObject& body, const QString& token,
                                         const QString& address = QStringLiteral("192.168.1.20"))
    {
        HttpRequest req;
        req.method = QStringLiteral("POST");
        req.path = QStringLiteral("/api/hosts/") + kHost + QStringLiteral("/start");
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        req.clientAddress = address;
        req.boothPin = true;
        req.adminLocked = true;
        if (!token.isEmpty()) req.headers.insert(QStringLiteral("x-session"), token);
        const BoothTakeover target{kHost, stream.key, QStringLiteral("PC-owner"), stream.appId};
        return boothTakeoverRefusal(pins, proofs, deps, req, body, target);
    }

    QJsonObject takeOverBody(const Browser& b, const QString& token, const QString& pin,
                             const QString& action = QStringLiteral("booth_takeover"),
                             const QString& host = kHost)
    {
        return {{QStringLiteral("appId"), 9},
                {QStringLiteral("booth_pin"), pin},
                {QStringLiteral("owner_proof"), proof(b, token, action, host)}};
    }
};

QString errorOf(const HttpResponse& r)
{
    return QJsonDocument::fromJson(r.body).object().value(QStringLiteral("error")).toString();
}

void testOnlyABoothWithPinsAnswers()
{
    SECTION("booth routes: only a booth with stream PINs on answers, for a host with a stream");
    Booth b;
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    for (const char* path : {"/api/booth/join", "/api/booth/pin"}) {
        const HttpResponse off =
            b.post(QString::fromLatin1(path), {{"host_uuid", kHost}}, QStringLiteral("sam"),
                   QStringLiteral("192.168.1.20"), false, false);
        CHECK_EQ(off.statusCode, 404);
        CHECK(errorOf(off) == QStringLiteral("not_found"));
        const HttpResponse noHost =
            b.post(QString::fromLatin1(path), QJsonObject(), QStringLiteral("sam"));
        CHECK_EQ(noHost.statusCode, 400);
    }
    // No stream on that host, or a host that gives each device its own session.
    b.stream.live = false;
    CHECK(errorOf(b.post("/api/booth/join", {{"host_uuid", kHost}}, "sam")) ==
          QStringLiteral("no_stream"));
    b.stream.live = true;
    b.stream.supported = false;
    const HttpResponse wolf = b.post("/api/booth/join", {{"host_uuid", kHost}}, "sam");
    CHECK_EQ(wolf.statusCode, 409);
    CHECK(errorOf(wolf) == QStringLiteral("booth_unsupported"));
    CHECK(errorOf(b.post("/api/booth/pin", {{"host_uuid", kHost}, {"action", "read"}}, "sam")) ==
          QStringLiteral("booth_unsupported"));
}

void testWhoMayJoin()
{
    SECTION("booth routes: a known device of another session joins; nobody else gets that far");
    Booth b;
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    const QString pin = b.pinNow();
    int status = 0;

    // No session: nothing to hold a seat by — this machine without one included.
    CHECK(errorOf(b.post("/api/booth/join", {{"host_uuid", kHost}, {"pin", pin}}, QString())) ==
          QStringLiteral("booth_session_required"));
    CHECK(errorOf(b.post("/api/booth/join", {{"host_uuid", kHost}, {"pin", pin}}, QString(),
                         QStringLiteral("127.0.0.1"), true)) ==
          QStringLiteral("booth_session_required"));
    // The one who started the stream does not join it.
    const Browser owner = newBrowser();
    b.keys.insert(QStringLiteral("owner-token"), owner.spki);
    CHECK(b.join(owner, QStringLiteral("owner-token"), pin, &status).value("error").toString() ==
          QStringLiteral("own_stream"));
    CHECK_EQ(status, 409);

    // Every refusal before the PIN leaves the PIN and its count alone.
    const quint64 g0 = b.pins.generation(b.stream.key);
    // No pairing key at all.
    const Browser keyless = newBrowser();
    CHECK(b.join(keyless, QStringLiteral("old"), pin, &status).value("error").toString() ==
          QStringLiteral("pairing_key_required"));
    CHECK_EQ(status, 403);
    // No proof.
    CHECK(errorOf(b.post("/api/booth/join", {{"host_uuid", kHost}, {"pin", pin}}, "sam")) ==
          QStringLiteral("owner_proof_required"));
    CHECK(b.reported.isEmpty());
    // A proof by another key, for another action, for another host.
    const Browser other = newBrowser();
    CHECK(b.join(other, QStringLiteral("sam"), pin, &status).value("error").toString() ==
          QStringLiteral("owner_proof_refused"));
    CHECK_EQ(b.reported.size(), 1);
    QJsonObject body{{QStringLiteral("host_uuid"), kHost},
                     {QStringLiteral("pin"), pin},
                     {QStringLiteral("owner_proof"), b.proof(sam, "sam", "booth_takeover")}};
    CHECK(errorOf(b.post("/api/booth/join", body, "sam")) == QStringLiteral("owner_proof_refused"));
    body[QStringLiteral("owner_proof")] =
        b.proof(sam, "sam", "booth_join", QStringLiteral("host-uuid-B"));
    CHECK(errorOf(b.post("/api/booth/join", body, "sam")) == QStringLiteral("owner_proof_refused"));
    // Five refusals and the PIN neither changed nor came closer to changing:
    // four wrong tries now still leave it in place.
    CHECK_EQ(b.pins.generation(b.stream.key), g0);
    CHECK(b.pins.read(b.stream.key) == pin);
    for (int i = 0; i < 4; ++i) {
        const Browser t = newBrowser();
        const QString token = QStringLiteral("t%1").arg(i);
        b.keys.insert(token, t.spki);
        CHECK(b.join(t, token,
                     QStringLiteral("000000") == pin ? QStringLiteral("000001")
                                                     : QStringLiteral("000000"),
                     &status, QStringLiteral("10.1.0.%1").arg(i))
                  .value("error")
                  .toString() == QStringLiteral("booth_pin_invalid"));
        CHECK_EQ(status, 401);
    }
    CHECK(b.pins.read(b.stream.key) == pin);
    // A wrong PIN is not reported to ConnectionGuard: a neighbour mistyping.
    CHECK_EQ(b.reported.size(), 3);
}

void testAJoinAndItsSeat()
{
    SECTION("booth routes: the right PIN seats the device, once, and the PIN moves on");
    Booth b;
    b.level = QStringLiteral("gamer");
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    const QString pin = b.pinNow();

    int status = 0;
    HttpResponse raw;
    const QJsonObject ok =
        b.join(sam, QStringLiteral("sam"), pin, &status, QStringLiteral("192.168.1.20"), &raw);
    CHECK_EQ(status, 200);
    const int slot = ok.value("slot").toInt(-1);
    CHECK(ShareManager::isPlayerSlot(slot));
    const QString url = ok.value("url").toString();
    CHECK(url.startsWith(QStringLiteral("/p/")));
    const QString token = url.mid(3);
    CHECK_EQ(b.share->slotForToken(token), slot);
    const QString setCookie = raw.headers.value(QStringLiteral("Set-Cookie"));
    CHECK(setCookie.startsWith(QStringLiteral("mw_player=")));
    CHECK(setCookie.contains(QStringLiteral("HttpOnly")));
    CHECK(setCookie.contains(QStringLiteral("Secure")));
    CHECK(setCookie.contains(QStringLiteral("SameSite=Strict")));
    const QString cookie = setCookie.mid(10).section(QLatin1Char(';'), 0, 0);
    CHECK_EQ(b.share->slotForCookie(cookie), slot);
    CHECK(raw.headers.value(QStringLiteral("Cache-Control")) == QStringLiteral("no-store"));
    // As the booth's level, under the machine's name, on the stream's app.
    CHECK(b.share->isBooth(slot));
    CHECK(b.share->permissions(slot) == boothPermissionsFor(QStringLiteral("gamer")));
    CHECK(b.share->status().at(slot - ShareManager::kFirstSlot).name == QStringLiteral("PC-sam"));
    CHECK_EQ(b.share->appForSlot(slot), 7);
    // Used: the PIN changed.
    CHECK(b.pins.read(b.stream.key) != pin);
    CHECK(b.pins.lastReason(b.stream.key) == BoothPin::Reason::Used);

    // The same session again: the same seat, a fresh cookie in place of the
    // old one — never a second seat.
    const QString pin2 = b.pins.read(b.stream.key);
    const QJsonObject again =
        b.join(sam, QStringLiteral("sam"), pin2, &status, QStringLiteral("192.168.1.20"), &raw);
    CHECK_EQ(status, 200);
    CHECK_EQ(again.value("slot").toInt(), slot);
    CHECK(again.value("url").toString() == url);
    // Nobody new came in: the PIN stays as it was.
    CHECK(b.pins.read(b.stream.key) == pin2);
    CHECK(b.pins.lastReason(b.stream.key) == BoothPin::Reason::Used);
    const QString cookie2 =
        raw.headers.value(QStringLiteral("Set-Cookie")).mid(10).section(QLatin1Char(';'), 0, 0);
    CHECK(cookie2 != cookie);
    CHECK_EQ(b.share->slotForCookie(cookie2), slot);
    CHECK_EQ(b.share->slotForCookie(cookie), -1);
    int seats = 0;
    for (const ShareManager::SlotStatus& st : b.share->status())
        seats += st.booth ? 1 : 0;
    CHECK_EQ(seats, 1);

    // Every seat taken: room full, and the right PIN is not spent on it.
    for (int s = ShareManager::kFirstSlot; s <= ShareManager::kLastSlot; ++s) {
        if (s == slot) continue;
        QString t, c;
        CHECK(b.share->activateBound(s, kHost, 7, {}, QStringLiteral("X"), QString(), t, c));
    }
    const Browser alex = newBrowser();
    b.keys.insert(QStringLiteral("alex"), alex.spki);
    const QString pin3 = b.pins.read(b.stream.key);
    CHECK(b.join(alex, QStringLiteral("alex"), pin3, &status).value("error").toString() ==
          QStringLiteral("room_full"));
    CHECK_EQ(status, 503);
    CHECK(b.pins.read(b.stream.key) == pin3);
}

void testHeldBackBeforeTheProof()
{
    SECTION("booth routes: a caller held back is refused before its proof is spent");
    Booth b;
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    const QString pin = b.pinNow();
    int status = 0;
    for (int i = 0; i < 3; ++i)
        b.join(sam, QStringLiteral("sam"), QStringLiteral("x"), &status,
               QStringLiteral("10.2.0.%1").arg(i));
    // The bucket is shut: the proof sent with this one is not even looked at,
    // so it is still good once the bucket opens.
    const QJsonObject p = b.proof(sam, QStringLiteral("sam"), "booth_join");
    const HttpResponse held =
        b.post("/api/booth/join", {{"host_uuid", kHost}, {"pin", pin}, {"owner_proof", p}},
               QStringLiteral("sam"));
    CHECK_EQ(held.statusCode, 429);
    CHECK(QJsonDocument::fromJson(held.body).object().value("lockout_seconds").toInt() > 0);
    CHECK_EQ(b.proofs.pendingCount(), 1);
}

void testWhoReadsThePin()
{
    SECTION("booth routes: the PIN, to whoever started the stream and nobody else");
    Booth b;
    const Browser owner = newBrowser();
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("owner-token"), owner.spki);
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    const QString pin = b.pinNow();

    auto read = [&b](const Browser& by, const QString& token, const QString& action,
                     const QString& proofAction) {
        return b.post("/api/booth/pin",
                      {{"host_uuid", kHost},
                       {"action", action},
                       {"owner_proof", b.proof(by, token, proofAction)}},
                      token);
    };
    HttpResponse r = read(owner, QStringLiteral("owner-token"), "read", "booth_pin_read");
    CHECK_EQ(r.statusCode, 200);
    QJsonObject obj = QJsonDocument::fromJson(r.body).object();
    CHECK(obj.value("pin").toString() == pin);
    CHECK(r.headers.value(QStringLiteral("Cache-Control")) == QStringLiteral("no-store"));
    // Another session, with a good proof of its own: not its stream.
    r = read(sam, QStringLiteral("sam"), "read", "booth_pin_read");
    CHECK_EQ(r.statusCode, 403);
    CHECK(errorOf(r) == QStringLiteral("booth_owner_only"));
    CHECK(!r.body.contains(pin.toUtf8()));
    // The owner's proof for reading does not ask for a new one.
    r = read(owner, QStringLiteral("owner-token"), "new", "booth_pin_read");
    CHECK(errorOf(r) == QStringLiteral("owner_proof_refused"));
    CHECK(b.pins.read(b.stream.key) == pin);
    r = read(owner, QStringLiteral("owner-token"), "new", "booth_pin_new");
    obj = QJsonDocument::fromJson(r.body).object();
    CHECK(obj.value("pin").toString() != pin);
    CHECK(obj.value("pin").toString() == b.pins.read(b.stream.key));
    CHECK(b.pins.lastReason(b.stream.key) == BoothPin::Reason::Asked);
    CHECK(errorOf(read(owner, QStringLiteral("owner-token"), "both", "booth_pin_read")) ==
          QStringLiteral("action_unknown"));
    // No session at all.
    CHECK(errorOf(b.post("/api/booth/pin", {{"host_uuid", kHost}, {"action", "read"}},
                         QString())) == QStringLiteral("booth_owner_only"));

    // This machine's own stream: read there without a proof, it has no key and
    // cannot be anybody else. From anywhere else, never as "this machine".
    b.stream.owner = StreamOwner::of(true, QString());
    r = b.post("/api/booth/pin", {{"host_uuid", kHost}, {"action", "read"}}, QString(),
               QStringLiteral("127.0.0.1"), true);
    CHECK_EQ(r.statusCode, 200);
    CHECK(QJsonDocument::fromJson(r.body).object().value("pin").toString() ==
          b.pins.read(b.stream.key));
    CHECK(errorOf(read(owner, QStringLiteral("owner-token"), "read", "booth_pin_read")) ==
          QStringLiteral("booth_owner_only"));
}

void testATakeOverWithThePin()
{
    SECTION("booth routes: a take-over asks for the PIN, then takes the join's steps");
    Booth b;
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    const QString pin = b.pinNow();
    const quint64 g0 = b.pins.generation(b.stream.key);
    const QString wrong =
        QStringLiteral("000000") == pin ? QStringLiteral("000001") : QStringLiteral("000000");

    // Asked first: whose stream it is and what it streams, nothing counted,
    // no proof spent. A PIN under the join's name is no take-over's PIN.
    for (const QJsonObject& body :
         {QJsonObject{{"appId", 9}}, QJsonObject{{"appId", 9}, {"pin", pin}}}) {
        const std::optional<HttpResponse> asked = b.takeOver(body, QStringLiteral("sam"));
        CHECK(asked.has_value());
        if (!asked) continue;
        CHECK_EQ(asked->statusCode, 409);
        const QJsonObject obj = QJsonDocument::fromJson(asked->body).object();
        CHECK(obj.value("error").toString() == QStringLiteral("booth_pin_required"));
        CHECK(obj.value("owner").toString() == QStringLiteral("PC-owner"));
        CHECK_EQ(obj.value("app_id").toInt(), 7);
        CHECK(!asked->body.contains(pin.toUtf8()));
    }
    CHECK_EQ(b.pins.generation(b.stream.key), g0);
    CHECK_EQ(b.proofs.pendingCount(), 0);

    // No session to hold the tries by.
    std::optional<HttpResponse> r = b.takeOver({{"booth_pin", pin}}, QString());
    CHECK(r && r->statusCode == 403 && errorOf(*r) == QStringLiteral("booth_session_required"));
    // A proof for a join, or for another host, opens no take-over; nor does
    // none. None of it touches the PIN.
    r = b.takeOver(b.takeOverBody(sam, QStringLiteral("sam"), pin, QStringLiteral("booth_join")),
                   QStringLiteral("sam"));
    CHECK(r && errorOf(*r) == QStringLiteral("owner_proof_refused"));
    r = b.takeOver(b.takeOverBody(sam, QStringLiteral("sam"), pin, QStringLiteral("booth_takeover"),
                                  QStringLiteral("host-uuid-B")),
                   QStringLiteral("sam"));
    CHECK(r && errorOf(*r) == QStringLiteral("owner_proof_refused"));
    r = b.takeOver({{"booth_pin", pin}}, QStringLiteral("sam"));
    CHECK(r && errorOf(*r) == QStringLiteral("owner_proof_required"));
    CHECK_EQ(b.pins.generation(b.stream.key), g0);
    CHECK(b.pins.read(b.stream.key) == pin);

    // A wrong PIN: the tries left, and the PIN where it was.
    r = b.takeOver(b.takeOverBody(sam, QStringLiteral("sam"), wrong), QStringLiteral("sam"));
    CHECK(r && r->statusCode == 401);
    if (r) {
        const QJsonObject obj = QJsonDocument::fromJson(r->body).object();
        CHECK(obj.value("error").toString() == QStringLiteral("booth_pin_invalid"));
        // Three tries before the first wait, one of them spent.
        CHECK_EQ(obj.value("remaining").toInt(), 2);
        CHECK(!r->body.contains(pin.toUtf8()));
    }
    CHECK(b.pins.read(b.stream.key) == pin);

    // The right one: let through, and the PIN it used is spent.
    r = b.takeOver(b.takeOverBody(sam, QStringLiteral("sam"), pin), QStringLiteral("sam"),
                   QStringLiteral("192.168.1.21"));
    CHECK(!r.has_value());
    CHECK(b.pins.read(b.stream.key) != pin);
    CHECK_EQ(b.pins.generation(b.stream.key), g0 + 1);
    CHECK(b.pins.lastReason(b.stream.key) == BoothPin::Reason::Used);
    r = b.takeOver(b.takeOverBody(sam, QStringLiteral("sam"), pin), QStringLiteral("sam"),
                   QStringLiteral("192.168.1.22"));
    CHECK(r && errorOf(*r) == QStringLiteral("booth_pin_invalid"));
}

void testATakeOverOfAStreamNotUpYet()
{
    SECTION("booth routes: a stream not up yet has no PIN to try, and nothing is counted");
    Booth b;
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    b.stream.live = false;
    // One proof, sent again and again: never looked at, so never spent.
    const QJsonObject body = b.takeOverBody(sam, QStringLiteral("sam"), QStringLiteral("000000"));
    for (int i = 0; i < 6; ++i) {
        const std::optional<HttpResponse> r = b.takeOver(body, QStringLiteral("sam"));
        CHECK(r && r->statusCode == 409 && errorOf(*r) == QStringLiteral("booth_pin_required"));
    }
    // No bucket moved, no PIN made for it, the proof still unspent.
    CHECK_EQ(b.pins.heldFor(QStringLiteral("sam"), QStringLiteral("192.168.1.20")), 0);
    CHECK(!b.pins.has(b.stream.key));
    CHECK_EQ(b.proofs.pendingCount(), 1);
    CHECK(b.reported.isEmpty());
}

void testATakeOverHeldBackFirst()
{
    SECTION("booth routes: a take-over held back is refused before its proof is spent");
    Booth b;
    const Browser sam = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    const QString pin = b.pinNow();
    // Three wrong joins from this session shut its bucket: a take-over from it
    // is held back too, its proof left unspent and its PIN unread.
    int status = 0;
    for (int i = 0; i < 3; ++i)
        b.join(sam, QStringLiteral("sam"), QStringLiteral("x"), &status,
               QStringLiteral("10.4.0.%1").arg(i));
    const std::optional<HttpResponse> held =
        b.takeOver(b.takeOverBody(sam, QStringLiteral("sam"), pin), QStringLiteral("sam"));
    CHECK(held && held->statusCode == 429);
    CHECK_EQ(b.proofs.pendingCount(), 1);
    CHECK(b.pins.read(b.stream.key) == pin);
}

void testThePinNeverLeavesByTheWrongDoor()
{
    SECTION("booth routes: no PIN, token or cookie in a log line or an error body");
    Booth b;
    const Browser sam = newBrowser();
    const Browser owner = newBrowser();
    b.keys.insert(QStringLiteral("sam"), sam.spki);
    b.keys.insert(QStringLiteral("owner-token"), owner.spki);
    QStringList lines;
    Logger::instance()->setTap(
        [&lines](Logger::Level, const QString& line) { lines.append(line); });

    const QString pin = b.pinNow();
    QStringList bodies;
    int status = 0;
    HttpResponse raw;
    // Refusals of every kind, then a join, then the owner's reads.
    b.join(newBrowser(), QStringLiteral("sam"), pin, &status, QStringLiteral("10.3.0.1"), &raw);
    bodies.append(QString::fromUtf8(raw.body));
    b.join(sam, QStringLiteral("sam"), QStringLiteral("000000"), &status,
           QStringLiteral("10.3.0.2"), &raw);
    bodies.append(QString::fromUtf8(raw.body));
    const QJsonObject ok =
        b.join(sam, QStringLiteral("sam"), pin, &status, QStringLiteral("10.3.0.3"), &raw);
    const QString cookie =
        raw.headers.value(QStringLiteral("Set-Cookie")).mid(10).section(QLatin1Char(';'), 0, 0);
    const QString link = ok.value("url").toString().mid(3);
    // A take-over asked, refused, then let through.
    for (const QString& tried : {QString(), QStringLiteral("000000"), pin}) {
        const QJsonObject body = tried.isEmpty()
                                     ? QJsonObject{{"appId", 9}}
                                     : b.takeOverBody(sam, QStringLiteral("sam"), tried);
        if (const std::optional<HttpResponse> r =
                b.takeOver(body, QStringLiteral("sam"), QStringLiteral("10.3.0.4")))
            bodies.append(QString::fromUtf8(r->body));
    }
    const HttpResponse fresh =
        b.post("/api/booth/pin",
               {{"host_uuid", kHost},
                {"action", "new"},
                {"owner_proof", b.proof(owner, QStringLiteral("owner-token"), "booth_pin_new")}},
               QStringLiteral("owner-token"));
    const QString newPin = QJsonDocument::fromJson(fresh.body).object().value("pin").toString();
    Logger::instance()->setTap({});

    CHECK(!cookie.isEmpty());
    CHECK(!link.isEmpty());
    CHECK(!lines.isEmpty());
    for (const QString& line : lines) {
        CHECK(!line.contains(pin));
        CHECK(!line.contains(newPin));
        CHECK(!line.contains(cookie));
        CHECK(!line.contains(link));
        CHECK(!line.contains(QStringLiteral("owner-token")));
        CHECK(!line.contains(QStringLiteral("PC-sam"))); // the machine goes unnamed
    }
    for (const QString& body : bodies) {
        CHECK(!body.contains(pin));
        CHECK(!body.contains(newPin));
    }
}

} // namespace

void run_booth_routes_tests()
{
    testOnlyABoothWithPinsAnswers();
    testWhoMayJoin();
    testAJoinAndItsSeat();
    testHeldBackBeforeTheProof();
    testWhoReadsThePin();
    testATakeOverWithThePin();
    testATakeOverOfAStreamNotUpYet();
    testATakeOverHeldBackFirst();
    testThePinNeverLeavesByTheWrongDoor();
}
