/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * How a TV on plain HTTP is signed in (issue #42): the code it shows, the
 * claim it asks with, and the book apart from MW-PAIR-v1's.
 */

#include "test_framework.h"
#include "server/LanHttpPairing.h"
#include "server/PhonePairing.h"

#include <QSet>

namespace {

QByteArray noBytes(int)
{
    return {};
}

} // namespace

void run_lan_http_pairing_tests()
{
    SECTION("LanHttpPairing — what a TV is given");

    const QString tv = QStringLiteral("192.168.1.50");
    const QString other = QStringLiteral("192.168.1.51");
    const qint64 t0 = 1'000'000;

    {
        LanHttpPairing book;
        const LanHttpPairing::OpenResult r = book.open(tv, t0);
        CHECK(r.outcome == LanHttpPairing::Open::Opened);
        // An id of the shape the approval page reads, a code of six symbols of
        // Crockford's alphabet, a claim of 32 bytes.
        CHECK(PhonePairing::isRequestId(r.request.id));
        CHECK_EQ(r.request.code.size(), qsizetype(LanHttpPairing::CODE_LENGTH));
        for (const QChar c : r.request.code)
            CHECK(QStringLiteral("0123456789ABCDEFGHJKMNPQRSTVWXYZ").contains(c));
        CHECK_EQ(QByteArray::fromBase64(r.request.claim.toLatin1(), QByteArray::Base64UrlEncoding)
                     .size(),
                 qsizetype(LanHttpPairing::CLAIM_BYTES));
        CHECK_EQ(book.pending(t0), 1);
    }

    // Drawn afresh each time, from the system's random source.
    {
        LanHttpPairing book;
        QSet<QString> ids, claims;
        for (int i = 0; i < 4; ++i) {
            const auto r = book.open(QStringLiteral("192.168.1.%1").arg(60 + i), t0);
            CHECK(r.outcome == LanHttpPairing::Open::Opened);
            ids.insert(r.request.id);
            claims.insert(r.request.claim);
        }
        CHECK_EQ(ids.size(), qsizetype(4));
        CHECK_EQ(claims.size(), qsizetype(4));
    }

    // No random bytes, no request.
    {
        LanHttpPairing book(&noBytes);
        CHECK(book.open(tv, t0).outcome == LanHttpPairing::Open::BadSource);
        CHECK_EQ(book.pending(t0), 0);
    }

    SECTION("LanHttpPairing — the book's places");

    {
        LanHttpPairing book;
        for (int i = 0; i < LanHttpPairing::MAX_PENDING; ++i)
            CHECK(book.open(QStringLiteral("192.168.1.%1").arg(100 + i), t0).outcome ==
                  LanHttpPairing::Open::Opened);
        CHECK(book.open(tv, t0).outcome == LanHttpPairing::Open::Full);
        // One address holds one place: asking again replaces its own request.
        const auto again = book.open(QStringLiteral("192.168.1.100"), t0 + 1);
        CHECK(again.outcome == LanHttpPairing::Open::Opened);
        CHECK_EQ(book.pending(t0 + 1), LanHttpPairing::MAX_PENDING);
        // Places free up as requests expire.
        CHECK(book.open(tv, t0 + LanHttpPairing::TTL_SECS + 1).outcome ==
              LanHttpPairing::Open::Opened);
    }

    SECTION("LanHttpPairing — approved, by the code the TV shows");

    {
        LanHttpPairing book;
        const auto r = book.open(tv, t0).request;
        // The approval page shows the TV's address, never the code.
        const auto shown = book.find(r.id, t0 + 10);
        CHECK(shown.has_value());
        CHECK_EQ(shown->address, tv);
        CHECK_EQ(shown->expiresIn, LanHttpPairing::TTL_SECS - 10);
        CHECK(!book.find(QStringLiteral("AAAAAAAAAAAAAAAA"), t0).has_value());

        CHECK(book.poll(r.id, r.claim, tv, t0 + 1).state == LanHttpPairing::Poll::Pending);
        // Typed as people type it: lower case, a space, an O for a zero.
        QString typed = r.code.toLower();
        typed.insert(3, QLatin1Char(' '));
        typed.replace(QLatin1Char('0'), QLatin1Char('o'));
        CHECK(book.approve(r.id, typed, QStringLiteral("approver-id"), t0 + 2) ==
              LanHttpPairing::Answer::Approved);
        // Answered once, with who approved; then spent.
        const auto first = book.poll(r.id, r.claim, tv, t0 + 3);
        CHECK(first.state == LanHttpPairing::Poll::Approved);
        CHECK_EQ(first.approverId, QStringLiteral("approver-id"));
        CHECK(book.poll(r.id, r.claim, tv, t0 + 4).state == LanHttpPairing::Poll::Gone);
        CHECK(book.approve(r.id, r.code, QStringLiteral("x"), t0 + 4) ==
              LanHttpPairing::Answer::Gone);
    }

    SECTION("LanHttpPairing — only the page that asked, from its address");

    {
        LanHttpPairing book;
        const auto r = book.open(tv, t0).request;
        CHECK(book.approve(r.id, r.code, QString(), t0 + 1) == LanHttpPairing::Answer::Approved);
        // Another address, a wrong claim, no claim: as if there were nothing.
        CHECK(book.poll(r.id, r.claim, other, t0 + 2).state == LanHttpPairing::Poll::Gone);
        // Wrong by its first character, whatever that is: an "x" put there blindly
        // left a claim drawn with an "x" right, once in 64 runs.
        QString wrongClaim = r.claim;
        wrongClaim[0] = wrongClaim[0] == QLatin1Char('x') ? QLatin1Char('y') : QLatin1Char('x');
        CHECK(book.poll(r.id, wrongClaim, tv, t0 + 2).state == LanHttpPairing::Poll::Gone);
        CHECK(book.poll(r.id, QString(), tv, t0 + 2).state == LanHttpPairing::Poll::Gone);
        // Still there for the right one, approved by the host machine.
        const auto right = book.poll(r.id, r.claim, tv, t0 + 3);
        CHECK(right.state == LanHttpPairing::Poll::Approved);
        CHECK(right.approverId.isEmpty());
    }

    SECTION("LanHttpPairing — wrong codes, refusals and time");

    {
        LanHttpPairing book;
        const auto r = book.open(tv, t0).request;
        QString wrong = r.code;
        wrong[0] = wrong[0] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        CHECK(book.approve(r.id, wrong, QString(), t0 + 1) == LanHttpPairing::Answer::WrongCode);
        CHECK(book.approve(r.id, wrong, QString(), t0 + 2) == LanHttpPairing::Answer::WrongCode);
        // The third wrong code drops the request: the right one is too late.
        CHECK(book.approve(r.id, wrong, QString(), t0 + 3) == LanHttpPairing::Answer::Gone);
        CHECK(book.approve(r.id, r.code, QString(), t0 + 4) == LanHttpPairing::Answer::Gone);
        CHECK(book.poll(r.id, r.claim, tv, t0 + 5).state == LanHttpPairing::Poll::Gone);
        // A code of another length is a wrong code, not a match on its prefix.
        const auto s = book.open(tv, t0 + 6).request;
        CHECK(book.approve(s.id, s.code.left(5), QString(), t0 + 7) ==
              LanHttpPairing::Answer::WrongCode);
        CHECK(book.approve(s.id, s.code + QLatin1Char('0'), QString(), t0 + 7) ==
              LanHttpPairing::Answer::WrongCode);
    }
    {
        LanHttpPairing book;
        const auto r = book.open(tv, t0).request;
        CHECK(book.refuse(r.id, t0 + 1));
        CHECK(!book.refuse(r.id, t0 + 1));
        CHECK(!book.find(r.id, t0 + 1).has_value());
        CHECK(book.approve(r.id, r.code, QString(), t0 + 2) == LanHttpPairing::Answer::Gone);
        CHECK(book.poll(r.id, r.claim, tv, t0 + 3).state == LanHttpPairing::Poll::Refused);
        CHECK(book.poll(r.id, r.claim, tv, t0 + 4).state == LanHttpPairing::Poll::Gone);
    }
    {
        LanHttpPairing book;
        const auto r = book.open(tv, t0).request;
        const qint64 late = t0 + LanHttpPairing::TTL_SECS + 1;
        CHECK(!book.find(r.id, late).has_value());
        CHECK(book.approve(r.id, r.code, QString(), late) == LanHttpPairing::Answer::Gone);
        CHECK(book.poll(r.id, r.claim, tv, late).state == LanHttpPairing::Poll::Gone);
        // An approval not collected in time is lost with its request.
        const auto s = book.open(tv, t0).request;
        CHECK(book.approve(s.id, s.code, QString(), t0 + 1) == LanHttpPairing::Answer::Approved);
        CHECK(book.poll(s.id, s.claim, tv, late).state == LanHttpPairing::Poll::Gone);
    }

    SECTION("LanHttpPairing — apart from MW-PAIR-v1's book");

    {
        // A TV's request is unknown to the phone pairing book, and a phone's
        // request is unknown to this one: neither book reads the other.
        LanHttpPairing tvBook;
        PhonePairing phoneBook;
        const auto r = tvBook.open(tv, t0).request;
        PhonePairing::Request seen;
        CHECK(phoneBook.find(r.id, t0 + 1, &seen) == PhonePairing::Lookup::Unknown);
        CHECK(phoneBook.approve(r.id, r.code, QString(), t0 + 1).outcome ==
              PhonePairing::Answer::Unknown);
        CHECK_EQ(tvBook.pending(t0 + 1), 1);
        CHECK(tvBook.poll(r.id, r.claim, tv, t0 + 1).state == LanHttpPairing::Poll::Pending);
    }
}
