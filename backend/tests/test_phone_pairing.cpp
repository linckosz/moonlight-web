/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/PairingCrypto.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/PhonePairing.h"

#include <QCryptographicHash>
#include <QStringList>
#include <QTemporaryDir>

namespace {

using Claim = PhonePairing::Claim;
using Answer = PhonePairing::Answer;
using Lookup = PhonePairing::Lookup;
using Open = PhonePairing::Open;

constexpr qint64 kT0 = 1'800'000'000;
const QString kHostId = QStringLiteral("host-key-id");
const QString kHome = QStringLiteral("192.168.1.174");
const QString kPhone = QStringLiteral("phone-session-id");

/// @p count bytes, the i-th being @p at(i): the vectors the TV app's tests share
/// (top.moonlightweb.tv.net.PhonePairingTest).
QByteArray bytes(int count, int (*at)(int))
{
    QByteArray out(count, '\0');
    for (int i = 0; i < count; ++i)
        out[i] = static_cast<char>(at(i) & 0xff);
    return out;
}

/// This host's MW-BIND-v1 key, made once for the whole suite.
const QByteArray& hostKey()
{
    static const QByteArray key =
        PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
    return key;
}

/// A device: its private key, its public one, a nonce and what commits to it.
struct Device
{
    QByteArray pem = PairingCrypto::generatePrivateKeyPem();
    QByteArray spki = PairingCrypto::publicKeySpkiFromPrivatePem(pem);
    QByteArray nonce = PairingCrypto::generateNonce();
    QByteArray commitment = PhonePairing::commitment(nonce);

    QByteArray sign(const QString& id, const QByteArray& n) const
    {
        return PairingCrypto::sign(pem, PhonePairing::claimMessage(kHostId, id, n));
    }
};

/// A claim as the routes make it, off the tunnel unless @p viaTunnel.
Claim claimWith(PhonePairing& book, const QString& id, const QByteArray& nonce,
                const QByteArray& sig, qint64 now = kT0 + 1, bool viaTunnel = false,
                const QByteArray& tunnelKey = QByteArray())
{
    return book.claim(id, nonce, sig, kHostId, hostKey(), viaTunnel, tunnelKey, now).outcome;
}

/// The device's own claim, as it makes it every two seconds, on a socket.
PhonePairing::ClaimResult claim(PhonePairing& book, const Device& d, const QString& id,
                                qint64 now = kT0 + 1)
{
    return book.claim(id, d.nonce, d.sign(id, d.nonce), kHostId, hostKey(), false, QByteArray(),
                      now);
}

/// A device asks to pair from @p address, on a socket; the request's id, empty if refused.
QString ask(PhonePairing& book, const Device& d, const QString& address = kHome, qint64 now = kT0)
{
    const auto r = book.open(d.spki, "MoonlightWeb TV (MiTV)", d.commitment, address, address,
                             false, true, now);
    return r.outcome == Open::Full ? QString() : r.request.id;
}

QString codeOf(const Device& d, const QString& id)
{
    return PhonePairing::code(hostKey(), d.spki, id, d.nonce);
}

/// A code that is not @p code: what a person who misread it would type.
QString otherThan(const QString& code)
{
    return code == QLatin1String("0000") ? QStringLiteral("1111") : QStringLiteral("0000");
}

void testTheVectorsTheTvShares()
{
    SECTION("phone pairing: the code, the commitment and the claim, byte for byte as the TV");
    const QByteArray host = bytes(91, [](int i) { return i; });
    const QByteArray device = bytes(91, [](int i) { return i * 7 + 3; });
    const QString id = QStringLiteral("AbCdEfGhIjKlMnOp");
    const QByteArray nonce(32, '\x2a');
    CHECK_EQ(PhonePairing::commitment(nonce).toHex(),
             QByteArray("c2c50d38188bbc5349dd84d416240afc05d1b6c18b3acdc7657d8be4776aad88"));
    CHECK_EQ(PhonePairing::code(host, device, id, nonce), QStringLiteral("HJDS"));
    // Another nonce, or the keys the other way round, and the code changes.
    const QByteArray other(32, '\x2b');
    CHECK_EQ(PhonePairing::code(host, device, id, other), QStringLiteral("F22N"));
    CHECK_EQ(PhonePairing::code(device, host, id, nonce), QStringLiteral("4RBA"));
    // What the TV signs on each claim, hashed as its test hashes it.
    const QByteArray claim = PhonePairing::claimMessage(QStringLiteral("hostid"), id, nonce);
    CHECK_EQ(claim.size(), 86);
    CHECK_EQ(QCryptographicHash::hash(claim, QCryptographicHash::Sha256).toHex(),
             QByteArray("d0b64a87987727639c5f37754ab83fe63195066ab3cea66ebb0cfbe4cf030b02"));
}

void testTheVectorThePageShares()
{
    SECTION("phone pairing: the approval, byte for byte as the page frames it");
    // frontend/test/pairApprovalMessage.test.js holds the same bytes.
    const QString id = QStringLiteral("AbCdEfGhIjKlMnOp");
    CHECK_EQ(PhonePairing::approveMessage(kHostId, id, QStringLiteral("HJDS")).toHex(),
             QByteArray("000000124d572d504149522d76317c617070726f7665"
                        "0000000b686f73742d6b65792d6964"
                        "000000104162436445664768496a4b6c4d6e4f70"
                        "00000004484a4453"));
    // The refusal, framed the same way without a code (pairRefusalMessage).
    CHECK_EQ(PhonePairing::refuseMessage(kHostId, id).toHex(),
             QByteArray("000000114d572d504149522d76317c726566757365"
                        "0000000b686f73742d6b65792d6964"
                        "000000104162436445664768496a4b6c4d6e4f70"));
}

void testWhatEachSignatureIsFor()
{
    SECTION("phone pairing: a signature of one message is good for no other");
    const Device d;
    const QString id = QStringLiteral("AbCdEfGhIjKlMnOp");
    const QByteArray sig = d.sign(id, d.nonce);
    CHECK(PairingCrypto::verify(d.spki, PhonePairing::claimMessage(kHostId, id, d.nonce), sig));
    CHECK(
        !PairingCrypto::verify(d.spki, PhonePairing::claimMessage("elsewhere", id, d.nonce), sig));
    const QString asCode = QString::fromLatin1(d.nonce.toHex());
    CHECK(!PairingCrypto::verify(d.spki, PhonePairing::approveMessage(kHostId, id, asCode), sig));
    CHECK(PhonePairing::claimMessage(kHostId, id, "x") !=
          PhonePairing::approveMessage(kHostId, id, "x"));
}

void testShapesNamesAndCodes()
{
    SECTION("phone pairing: request ids, device names, codes as typed");
    PhonePairing book;
    const Device d;
    const QString id = ask(book, d);
    CHECK_EQ(id.size(), 16);
    CHECK(PhonePairing::isRequestId(id));
    CHECK(!PhonePairing::isRequestId(QStringLiteral("short")));
    CHECK(!PhonePairing::isRequestId(QStringLiteral("AbCdEfGhIjKlMnOp/../x")));
    CHECK(!PhonePairing::isRequestId(QStringLiteral("AbCdEfGhIjKlMnO=")));

    CHECK_EQ(PhonePairing::cleanName(QStringLiteral("  Salon\n TV\t ")),
             QStringLiteral("Salon TV"));
    const QString longName(200, QLatin1Char('x'));
    CHECK_EQ(PhonePairing::cleanName(longName).size(), PhonePairing::MAX_NAME);
    CHECK(!PhonePairing::cleanName(QStringLiteral("a\x1b[31mb")).contains(QChar(0x1b)));
    // A character beyond the first plane is kept whole, and never cut in half.
    const QString tv = QStringLiteral("TV \U0001F4FA");
    CHECK_EQ(PhonePairing::cleanName(tv), tv);
    const QString cut = QString(PhonePairing::MAX_NAME - 1, QLatin1Char('x')) + tv.right(2);
    CHECK_EQ(PhonePairing::cleanName(cut), QString(PhonePairing::MAX_NAME - 1, QLatin1Char('x')));

    // A code typed on a phone: case, spaces and dashes, and the letters read as digits.
    CHECK_EQ(PhonePairing::normalizeCode(QStringLiteral(" hj-ds ")), QStringLiteral("HJDS"));
    CHECK_EQ(PhonePairing::normalizeCode(QStringLiteral("o1Il")), QStringLiteral("0111"));
}

void testTypedApprovedClaimedThenSpent()
{
    SECTION("phone pairing: asked, revealed, its code typed, claimed, then spent");
    PhonePairing book;
    const Device d;
    const QString id = ask(book, d);
    CHECK(!id.isEmpty());

    // Before the device's first claim, there is no code to approve.
    PhonePairing::Request seen;
    CHECK(book.find(id, kT0, &seen) == Lookup::Found);
    CHECK(seen.code.isEmpty());
    CHECK(book.approve(id, QStringLiteral("HJDS"), kPhone, kT0).outcome == Answer::NotReady);

    CHECK(claim(book, d, id).outcome == Claim::Waiting);
    CHECK(book.find(id, kT0 + 2, &seen) == Lookup::Found);
    CHECK_EQ(seen.code, codeOf(d, id));
    CHECK_EQ(seen.machineName, QStringLiteral("MoonlightWeb TV (MiTV)"));

    // A wrong code leaves tries; the right one, typed loosely, approves.
    const PhonePairing::AnswerResult wrong =
        book.approve(id, otherThan(seen.code), kPhone, kT0 + 3);
    CHECK(wrong.outcome == Answer::CodeMismatch);
    CHECK_EQ(wrong.triesLeft, PhonePairing::CODE_TRIES - 1);
    const QString typed = QStringLiteral(" ") + seen.code.toLower() + QStringLiteral(" ");
    CHECK(book.approve(id, typed, kPhone, kT0 + 3).outcome == Answer::Done);
    CHECK(book.approve(id, seen.code, kPhone, kT0 + 3).outcome == Answer::Decided);

    const PhonePairing::ClaimResult paired = claim(book, d, id, kT0 + 4);
    CHECK(paired.outcome == Claim::Paired);
    CHECK_EQ(paired.request.deviceKey, d.spki);
    CHECK_EQ(paired.request.approver, kPhone);
    CHECK(paired.request.sessionId.isEmpty());
    book.markPaired(id, QStringLiteral("session-1"), kT0 + 4);

    // Its answer lost on the way, the device claims again: the session made
    // before comes back, for the routes to replace.
    const PhonePairing::ClaimResult again = claim(book, d, id, kT0 + 6);
    CHECK(again.outcome == Claim::Paired);
    CHECK_EQ(again.request.sessionId, QStringLiteral("session-1"));
    book.markPaired(id, QStringLiteral("session-2"), kT0 + 6);
    CHECK(book.approve(id, seen.code, kPhone, kT0 + 7).outcome == Answer::Decided);
    CHECK(book.refuse(id, kT0 + 7).outcome == Answer::Decided);

    // A minute later, nothing: one request, one session.
    const qint64 later = kT0 + 6 + PhonePairing::GRACE_SECS + 1;
    CHECK(claim(book, d, id, later).outcome == Claim::Unknown);
    CHECK_EQ(book.pending(), 0);
}

void testOnlyTheDeviceClaims()
{
    SECTION("phone pairing: whoever saw the QR code cannot claim, nor cancel");
    PhonePairing book;
    const Device d;
    const Device stranger;
    const QString id = ask(book, d);

    // The id is in the QR code; the device's key is not.
    CHECK(claimWith(book, id, d.nonce, stranger.sign(id, d.nonce)) == Claim::BadProof);
    CHECK(claimWith(book, id, d.nonce, QByteArray(64, '\0')) == Claim::BadProof);
    const QByteArray shortNonce(8, 'n');
    CHECK(claimWith(book, id, shortNonce, d.sign(id, shortNonce)) == Claim::BadProof);
    // Asked on a socket, it is claimed on a socket.
    CHECK(claimWith(book, id, d.nonce, d.sign(id, d.nonce), kT0 + 1, true, d.spki) ==
          Claim::BadProof);
    // ...and the request is still the device's.
    CHECK(claim(book, d, id).outcome == Claim::Waiting);

    // Asked on a line, claimed on that line, whose key must be the request's.
    const auto onLine = book.open(stranger.spki, "TV", stranger.commitment, "203.0.113.9",
                                  "203.0.113.9", true, false, kT0);
    const QString other = onLine.request.id;
    const QByteArray sig = stranger.sign(other, stranger.nonce);
    CHECK(claimWith(book, other, stranger.nonce, sig, kT0 + 1, true, d.spki) == Claim::BadProof);
    CHECK(claimWith(book, other, stranger.nonce, sig, kT0 + 1) == Claim::BadProof);
    CHECK(claimWith(book, other, stranger.nonce, sig, kT0 + 1, true, stranger.spki) ==
          Claim::Waiting);
}

void testTheNonceIsTheOneCommittedTo()
{
    SECTION("phone pairing: the device reveals the nonce it committed to, and keeps it");
    PhonePairing book;
    const Device d;
    const QByteArray otherNonce = PairingCrypto::generateNonce();

    // Its own key, another nonce than its commitment: the request ends.
    QString id = ask(book, d);
    CHECK(claimWith(book, id, otherNonce, d.sign(id, otherNonce)) == Claim::BadProof);
    CHECK(book.find(id, kT0 + 1, nullptr) == Lookup::Unknown);

    // Revealed once, it cannot change afterwards.
    id = ask(book, d);
    CHECK(claim(book, d, id).outcome == Claim::Waiting);
    CHECK(claimWith(book, id, otherNonce, d.sign(id, otherNonce), kT0 + 2) == Claim::BadProof);
    CHECK(book.find(id, kT0 + 2, nullptr) == Lookup::Unknown);
}

void testRefusedOnce()
{
    SECTION("phone pairing: refused, the device is told once");
    PhonePairing book;
    const Device d;
    const QString id = ask(book, d, QStringLiteral("192.168.1.50"));
    CHECK(claim(book, d, id).outcome == Claim::Waiting);
    const PhonePairing::AnswerResult no = book.refuse(id, kT0 + 2);
    CHECK(no.outcome == Answer::Done);
    CHECK_EQ(no.address, QStringLiteral("192.168.1.50"));
    CHECK(book.refuse(id, kT0 + 2).outcome == Answer::Decided);
    CHECK(book.approve(id, codeOf(d, id), kPhone, kT0 + 2).outcome == Answer::Decided);
    CHECK(claim(book, d, id, kT0 + 3).outcome == Claim::Refused);
    CHECK(claim(book, d, id, kT0 + 4).outcome == Claim::Unknown);
}

void testWrongCodesRefuse()
{
    SECTION("phone pairing: a code typed wrong too often refuses the request");
    PhonePairing book;
    const Device d;
    const QString id = ask(book, d, QStringLiteral("192.168.1.51"));
    CHECK(claim(book, d, id).outcome == Claim::Waiting);
    const QString misread = otherThan(codeOf(d, id));
    for (int i = 1; i < PhonePairing::CODE_TRIES; ++i) {
        const PhonePairing::AnswerResult a = book.approve(id, misread, kPhone, kT0 + 2);
        CHECK(a.outcome == Answer::CodeMismatch);
        CHECK_EQ(a.triesLeft, PhonePairing::CODE_TRIES - i);
    }
    const PhonePairing::AnswerResult last = book.approve(id, misread, kPhone, kT0 + 3);
    CHECK(last.outcome == Answer::TooManyTries);
    CHECK_EQ(last.address, QStringLiteral("192.168.1.51"));
    // The right code comes too late, and the device is told.
    CHECK(book.approve(id, codeOf(d, id), kPhone, kT0 + 4).outcome == Answer::Decided);
    CHECK(claim(book, d, id, kT0 + 5).outcome == Claim::Refused);
}

void testAFewMinutes()
{
    SECTION("phone pairing: five minutes, then a minute of 'expired', then nothing");
    PhonePairing book;
    const Device d;
    const QString id = ask(book, d);
    CHECK(claim(book, d, id, kT0 + 1).outcome == Claim::Waiting);
    const qint64 late = kT0 + PhonePairing::TTL_SECS + 1;
    CHECK(claim(book, d, id, late).outcome == Claim::Expired);
    CHECK(book.find(id, late, nullptr) == Lookup::Expired);
    CHECK(book.approve(id, codeOf(d, id), kPhone, late).outcome == Answer::Expired);
    const qint64 gone = late + PhonePairing::GRACE_SECS;
    CHECK(claim(book, d, id, gone).outcome == Claim::Unknown);
    CHECK_EQ(book.pending(), 0);

    // Approved but never claimed: nothing was made, and it expires the same.
    const QString approved = ask(book, d, kHome, gone);
    CHECK(claim(book, d, approved, gone + 1).outcome == Claim::Waiting);
    CHECK(book.approve(approved, codeOf(d, approved), kPhone, gone + 2).outcome == Answer::Done);
    const qint64 after = gone + PhonePairing::TTL_SECS + 1;
    CHECK(claim(book, d, approved, after).outcome == Claim::Expired);
}

void testAFewPerAddress()
{
    SECTION("phone pairing: one key, one request; a few per address; a ceiling");
    PhonePairing book;
    const Device d;
    const QString first = ask(book, d);
    const QString again = ask(book, d, kHome, kT0 + 1);
    CHECK(book.find(first, kT0 + 1, nullptr) == Lookup::Unknown);
    CHECK(book.find(again, kT0 + 1, nullptr) == Lookup::Found);
    // The same key named from another address cancels nothing.
    const QString elsewhere = ask(book, d, QStringLiteral("192.168.1.99"), kT0 + 2);
    CHECK(book.find(again, kT0 + 2, nullptr) == Lookup::Found);
    CHECK(book.find(elsewhere, kT0 + 2, nullptr) == Lookup::Found);

    // A fourth from one address pushes its oldest out, and says so.
    PhonePairing crowded;
    const Device a, b, c, e;
    const QString lan = QStringLiteral("10.0.0.9");
    const QString ia = ask(crowded, a, lan, kT0);
    const QString ib = ask(crowded, b, lan, kT0 + 1);
    const QString ic = ask(crowded, c, lan, kT0 + 2);
    const auto fourth = crowded.open(e.spki, "e", e.commitment, lan, lan, false, true, kT0 + 3);
    CHECK(fourth.outcome == Open::Crowded);
    CHECK(crowded.find(ia, kT0 + 3, nullptr) == Lookup::Unknown);
    CHECK(crowded.find(ib, kT0 + 3, nullptr) == Lookup::Found);
    CHECK(crowded.find(ic, kT0 + 3, nullptr) == Lookup::Found);
    CHECK(crowded.find(fourth.request.id, kT0 + 3, nullptr) == Lookup::Found);

    // Past the ceiling a new request is refused, and none already there is pushed out.
    PhonePairing full;
    QString kept;
    for (int i = 0; i < PhonePairing::MAX_PENDING; ++i) {
        const Device x;
        const QString id = ask(full, x, QStringLiteral("10.1.%1.1").arg(i), kT0);
        if (i == 0) kept = id;
    }
    CHECK_EQ(full.pending(), PhonePairing::MAX_PENDING);
    const Device last;
    const QString late = QStringLiteral("10.2.0.1");
    CHECK(full.open(last.spki, "last", last.commitment, late, late, false, true, kT0).outcome ==
          Open::Full);
    CHECK(full.find(kept, kT0, nullptr) == Lookup::Found);
    // Expired, they hold no place while they still answer "expired".
    const qint64 expired = kT0 + PhonePairing::TTL_SECS + 1;
    CHECK(full.open(last.spki, "last", last.commitment, late, late, false, true, expired).outcome ==
          Open::Opened);
    CHECK(full.find(kept, expired, nullptr) == Lookup::Expired);
}

void testBornInOneSecondTheyLeaveInTheirOrder()
{
    SECTION("phone pairing: born in the same second, an address's requests leave in their order");
    PhonePairing book;
    const QString lan = QStringLiteral("10.0.0.10");
    QStringList ids;
    for (int i = 0; i < PhonePairing::MAX_PER_ADDRESS + 2; ++i) {
        const Device x;
        const auto r = book.open(x.spki, "x", x.commitment, lan, lan, false, true, kT0);
        CHECK(r.outcome == (i < PhonePairing::MAX_PER_ADDRESS ? Open::Opened : Open::Crowded));
        ids.append(r.request.id);
    }
    // All born at kT0: the first two made room, the first first.
    CHECK(book.find(ids.at(0), kT0, nullptr) == Lookup::Unknown);
    CHECK(book.find(ids.at(1), kT0, nullptr) == Lookup::Unknown);
    for (int i = 2; i < ids.size(); ++i)
        CHECK(book.find(ids.at(i), kT0, nullptr) == Lookup::Found);
}

void testPlacesKeptForTheLan()
{
    SECTION("phone pairing: requests from elsewhere leave places for this LAN, and go on demand");
    PhonePairing book;
    const auto fromElsewhere = [&book](const Device& d, const QString& where) {
        return book.open(d.spki, "far", d.commitment, where, where, false, false, kT0);
    };
    // As many from elsewhere as they may hold, each from an address of its own.
    QString first;
    for (int i = 0; i < PhonePairing::MAX_FROM_ELSEWHERE; ++i) {
        const Device x;
        const auto r = fromElsewhere(x, QStringLiteral("198.51.100.%1").arg(i + 1));
        CHECK(r.outcome == Open::Opened);
        if (i == 0) first = r.request.id;
    }
    // One more from elsewhere is refused, as when the book is full.
    const Device more;
    CHECK(fromElsewhere(more, QStringLiteral("198.51.100.200")).outcome == Open::Full);
    // This LAN's devices still find the rest, up to the book's own ceiling.
    for (int i = PhonePairing::MAX_FROM_ELSEWHERE; i < PhonePairing::MAX_PENDING; ++i) {
        const Device x;
        CHECK(!ask(book, x, QStringLiteral("192.168.2.%1").arg(i), kT0).isEmpty());
    }
    CHECK_EQ(book.pending(), PhonePairing::MAX_PENDING);
    const Device last;
    CHECK(ask(book, last, QStringLiteral("192.168.3.1"), kT0).isEmpty());

    // Each keeps where it came from. Those from elsewhere go on demand, this
    // LAN's stay, and the places are free again.
    PhonePairing::Request seen;
    CHECK(book.find(first, kT0, &seen) == Lookup::Found);
    CHECK(!seen.fromLan);
    CHECK_EQ(seen.address, QStringLiteral("198.51.100.1"));
    CHECK(book.hasFromElsewhere());
    CHECK_EQ(book.dropFromElsewhere(), PhonePairing::MAX_FROM_ELSEWHERE);
    CHECK(!book.hasFromElsewhere());
    CHECK(book.find(first, kT0, nullptr) == Lookup::Unknown);
    CHECK_EQ(book.pending(), PhonePairing::MAX_PENDING - PhonePairing::MAX_FROM_ELSEWHERE);
    CHECK(fromElsewhere(more, QStringLiteral("198.51.100.200")).outcome == Open::Opened);
}

void testFailuresShareThePinsCount()
{
    SECTION("phone pairing: a failure counts like a wrong PIN, on the same counter");
    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";
    AuthManager auth(&settings);
    auth.generatePin();

    const QString ip = QStringLiteral("203.0.113.77");
    CHECK(auth.countPairingFailure(ip).result == AuthManager::InvalidPin);
    CHECK(!auth.isRateLimited(ip));
    auth.countPairingFailure(ip);
    // Two failed pairings and one wrong PIN: the PIN's first lockout.
    const auto third = auth.validatePin(ip, QStringLiteral("not-the-pin"));
    CHECK(third.lockoutSeconds > 0);
    CHECK(auth.isRateLimited(ip));
    CHECK(auth.validatePin(ip, auth.currentPin()).result == AuthManager::RateLimited);
    // Another address is not touched.
    CHECK(!auth.isRateLimited(QStringLiteral("203.0.113.78")));
}

void testAnApproverTakesWhatItApprovedAlong()
{
    SECTION("phone pairing: revoking a session revokes the ones it approved, and theirs");
    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";
    AuthManager auth(&settings);

    const QString phone = auth.createSession("192.168.1.20", "Phone");
    const QString tv = auth.createSession("192.168.1.174", "TV");
    const QString stick = auth.createSession("192.168.1.175", "Stick");
    const QString laptop = auth.createSession("192.168.1.30", "Laptop");
    const QString phoneId = auth.sessionIdForToken(phone);
    CHECK(auth.setSessionApprover(tv, phoneId));
    CHECK(auth.setSessionApprover(stick, auth.sessionIdForToken(tv)));
    CHECK(!auth.setSessionApprover(QStringLiteral("bogus-token"), phoneId));
    CHECK_EQ(auth.sessionForToken(tv).approvedBy, phoneId);
    CHECK_EQ(auth.sessionForToken(tv).toJson().value("approved_by").toString(), phoneId);
    CHECK(!auth.sessionForToken(laptop).toJson().contains("approved_by"));
    CHECK(auth.hasLiveSession(phoneId));

    auth.destroySession(phoneId);
    CHECK(!auth.hasLiveSession(phoneId));
    CHECK(!auth.validateSession(tv));
    CHECK(!auth.validateSession(stick));
    CHECK(auth.validateSession(laptop));

    // Logging out is not a revocation: what the session approved stays, now
    // the administrator's.
    const QString owner = auth.createSession("192.168.1.21", "Owner");
    const QString box = auth.createSession("192.168.1.176", "Box");
    CHECK(auth.setSessionApprover(box, auth.sessionIdForToken(owner)));
    CHECK(auth.logoutSession(owner));
    CHECK(!auth.validateSession(owner));
    CHECK(auth.validateSession(box));
    CHECK(auth.sessionForToken(box).approvedBy.isEmpty());
    auth.destroySession(auth.sessionIdForToken(box));
    auth.destroySession(auth.sessionIdForToken(laptop));
}

} // namespace

void run_phone_pairing_tests()
{
    testTheVectorsTheTvShares();
    testTheVectorThePageShares();
    testWhatEachSignatureIsFor();
    testShapesNamesAndCodes();
    testTypedApprovedClaimedThenSpent();
    testOnlyTheDeviceClaims();
    testTheNonceIsTheOneCommittedTo();
    testRefusedOnce();
    testWrongCodesRefuse();
    testAFewMinutes();
    testAFewPerAddress();
    testBornInOneSecondTheyLeaveInTheirOrder();
    testPlacesKeptForTheLan();
    testFailuresShareThePinsCount();
    testAnApproverTakesWhatItApprovedAlong();
}
