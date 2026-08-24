/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The rendezvous identifier — the address an instance is reached by now that
 * there is no sub-domain.
 *
 * The point of this file is the CROSS-IMPLEMENTATION contract. The same folding
 * rules exist twice: here, and in normaliseID() in
 * deploy/powerdns/mw-rendezvous/store.go, which is what the server keys its
 * ownership store on. If the two ever drift, a claim made under one spelling
 * becomes unfindable under the other and an instance silently loses its own
 * identifier — with no error anywhere, because both sides are individually
 * self-consistent. The pairs below are the same ones pinned by
 * TestNormaliseIDFoldsTheFormsAHumanProduces on the Go side; change one, change
 * both.
 */
#include "test_framework.h"

#include "../src/common/RendezvousId.h"

#include <QSet>

void run_rendezvous_id_tests()
{
    SECTION("RendezvousId — shape");

    // 26 characters is not arbitrary: it is 128 bits of entropy in a 32-symbol
    // alphabet. Shorter would make the set walkable, and the identifier is a
    // locator anyone may hold — access is granted by the pairing signature, not
    // by knowing this.
    const QString fresh = RendezvousId::generate();
    CHECK_EQ(fresh.size(), 26);
    CHECK(RendezvousId::isValid(fresh));

    // Crockford drops i, l, o and u so the value survives being read aloud.
    // A generator that emitted them would produce identifiers the server
    // rejects — and only for some draws, which is the worst way to find out.
    for (const QChar& c : fresh) {
        CHECK(c != QLatin1Char('i'));
        CHECK(c != QLatin1Char('l'));
        CHECK(c != QLatin1Char('o'));
        CHECK(c != QLatin1Char('u'));
    }

    SECTION("RendezvousId — draws differ");

    // A constant identifier would be catastrophic and completely silent: every
    // instance would claim the same value, the first would win, and the rest
    // would be unreachable forever. Cheap to check, so check it.
    QSet<QString> seen;
    for (int i = 0; i < 64; ++i) seen.insert(RendezvousId::generate());
    CHECK_EQ(seen.size(), 64);

    SECTION("RendezvousId — folding matches the server");

    const QString canonical = QStringLiteral("abcdefghjkmnpqrstvwxyz0123");

    // Case.
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("ABCDEFGHJKMNPQRSTVWXYZ0123")), canonical);
    // Grouping hyphens, the form a person would write down.
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("abcde-fghjk-mnpqr-stvwx-yz0123")), canonical);
    // Surrounding whitespace, the form a person would paste.
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("  abcdefghjkmnpqrstvwxyz0123  ")), canonical);

    const QString other = QStringLiteral("0123456789abcdefghjkmnpqrs");
    // Letter O reads as zero, I and L read as one, U reads as V.
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("O123456789abcdefghjkmnpqrs")), other);
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("0I23456789abcdefghjkmnpqrs")), other);
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("0l23456789abcdefghjkmnpqrs")), other);
    CHECK_EQ(RendezvousId::normalise(QStringLiteral("abcdefghjkmnpqrstvwxyz012u")),
             QStringLiteral("abcdefghjkmnpqrstvwxyz012v"));

    // Folding is idempotent — the canonical form is a fixed point. Without
    // this, a value could shift every time it passed through, and a stored
    // identifier would drift away from the claim it was granted for.
    CHECK_EQ(RendezvousId::normalise(canonical), canonical);
    CHECK_EQ(RendezvousId::normalise(RendezvousId::normalise(canonical)), canonical);

    SECTION("RendezvousId — validation");

    CHECK(RendezvousId::isValid(canonical));
    // Upper case is only valid AFTER folding: the server compares the folded
    // form, so accepting it raw here would let us claim under one spelling and
    // look ourselves up under another.
    CHECK(!RendezvousId::isValid(QStringLiteral("ABCDEFGHJKMNPQRSTVWXYZ0123")));
    CHECK(!RendezvousId::isValid(QString()));
    CHECK(!RendezvousId::isValid(QStringLiteral("abcdefghjkmnpqrstvwxyz012")));   // 25
    CHECK(!RendezvousId::isValid(QStringLiteral("abcdefghjkmnpqrstvwxyz01234"))); // 27
    CHECK(!RendezvousId::isValid(QStringLiteral("abcdefghjkmnpqrstvwxyz012!")));
    // The four ambiguous letters are not in the alphabet, so a value carrying
    // one is not valid until it has been folded.
    CHECK(!RendezvousId::isValid(QStringLiteral("abcdefghjkmnpqrstvwxyz012i")));
    CHECK(RendezvousId::isValidLoose(QStringLiteral("ABCDE-FGHJK-MNPQR-STVWX-YZ0123")));
    CHECK(!RendezvousId::isValidLoose(QStringLiteral("nope")));
}
