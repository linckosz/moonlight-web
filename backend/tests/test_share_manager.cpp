/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Session sharing: the state machine and the credentials that guard it.
 *
 * A share link is expected to travel over chat apps and screenshots, so what is
 * verified here is mostly what must NOT happen: a wrong PIN never gets in, a
 * revoked activation never comes back, a stream dropping never ends a share,
 * and permissions never change once a player could be acting on them.
 */
#include "test_framework.h"

#include "server/ShareManager.h"

#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QVector>

namespace {

const int kSlot = ShareManager::kFirstSlot;

// A share is always bound to the host the owner is streaming; these stand in for
// that context wherever a test does not care which host it is.
const QString kHost = QStringLiteral("host-uuid-A");
const int kApp = 42;
/// The lifetime every test but the TTL one takes for granted.
const qint64 kTtl = ShareManager::kTtlSecs;

/// A manager as it would be on a fresh install. The state file is removed
/// first, so remembered permissions from an earlier test — or from an earlier
/// run of this binary — cannot leak in. (The caller runs with QStandardPaths
/// test mode on, so this never touches the real profile.)
ShareManager* freshManager()
{
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                  QStringLiteral("/share.json"));
    return new ShareManager();
}

void test_activation_mints_distinct_secrets()
{
    SECTION("share: activation mints a distinct link and PIN each time");
    ShareManager* mgr = freshManager();

    QString token1, pin1;
    ShareManager::SlotStatus st1;
    CHECK(mgr->activate(kSlot, kHost, kApp, kTtl, token1, pin1, st1));
    CHECK(st1.state == ShareManager::State::Shared);
    // 32 random bytes, base64url, unpadded.
    CHECK(token1.size() >= 40);
    CHECK_EQ(pin1.size(), 6);
    CHECK(pin1.toInt() >= 0);

    QString token2, pin2;
    ShareManager::SlotStatus st2;
    CHECK(mgr->activate(kSlot, kHost, kApp, kTtl, token2, pin2, st2));
    CHECK(token1 != token2);
    // Re-sharing kills the old link outright — that is the only way to change
    // the permissions, so it must not leave the previous one usable.
    CHECK(!mgr->tokenIsLive(token1));
    CHECK(mgr->tokenIsLive(token2));

    // A slot outside the player range is not a slot.
    QString t3, p3;
    ShareManager::SlotStatus s3;
    CHECK(!mgr->activate(0, kHost, kApp, kTtl, t3, p3, s3));
    CHECK(!mgr->activate(ShareManager::kLastSlot + 1, kHost, kApp, kTtl, t3, p3, s3));
    // And an activation with no host to bind to is refused: an unbound link is
    // exactly the cross-host hole the binding closes.
    CHECK(!mgr->activate(kSlot, QString(), kApp, kTtl, t3, p3, s3));

    delete mgr;
}

void test_pin_is_required_and_bound_to_the_link()
{
    SECTION("share: the PIN is the second factor, and it is per-activation");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    // The link alone opens nothing.
    ShareManager::PinOutcome wrong = mgr->redeemPin(token, QStringLiteral("000000"), "1.2.3.4");
    if (pin == QStringLiteral("000000")) wrong.result = ShareManager::PinResult::Invalid; // 1e-6
    CHECK(wrong.result != ShareManager::PinResult::Ok);
    CHECK(wrong.cookie.isEmpty());

    // The PIN alone opens nothing either.
    const ShareManager::PinOutcome noToken = mgr->redeemPin(QStringLiteral("nope"), pin, "1.2.3.5");
    CHECK(noToken.result == ShareManager::PinResult::Invalid);

    // Both together do.
    const ShareManager::PinOutcome ok = mgr->redeemPin(token, pin, "1.2.3.6");
    CHECK(ok.result == ShareManager::PinResult::Ok);
    CHECK_EQ(ok.slot, kSlot);
    CHECK(!ok.cookie.isEmpty());
    CHECK_EQ(mgr->slotForCookie(ok.cookie), kSlot);

    // A cookie is worthless once the owner re-shares.
    QString token2, pin2;
    mgr->activate(kSlot, kHost, kApp, kTtl, token2, pin2, st);
    CHECK_EQ(mgr->slotForCookie(ok.cookie), -1);

    delete mgr;
}

void test_pin_bruteforce_kills_the_activation()
{
    SECTION("share: repeated wrong PINs destroy the invitation, never open it");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    // Each attempt comes from its own bucket so the per-caller lockout never
    // fires — this is the attacker with a botnet, and the per-activation
    // counter is what stops them.
    for (int i = 0; i < ShareManager::kMaxPinFailures; ++i) {
        const QString wrong =
            QStringLiteral("%1").arg((pin.toInt() + i + 1) % 1000000, 6, 10, QLatin1Char('0'));
        const ShareManager::PinOutcome out =
            mgr->redeemPin(token, wrong, QStringLiteral("10.0.0.%1").arg(i));
        CHECK(out.result != ShareManager::PinResult::Ok);
    }

    // The invitation is gone — even the real PIN is now useless.
    CHECK(!mgr->tokenIsLive(token));
    CHECK(mgr->state(kSlot) == ShareManager::State::Off);
    const ShareManager::PinOutcome late = mgr->redeemPin(token, pin, "10.0.1.1");
    CHECK(late.result == ShareManager::PinResult::Invalid);

    delete mgr;
}

void test_rate_limit_locks_a_single_caller()
{
    SECTION("share: one caller hammering gets locked out");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    bool sawLockout = false;
    for (int i = 0; i < 4; ++i) {
        const QString wrong =
            QStringLiteral("%1").arg((pin.toInt() + i + 1) % 1000000, 6, 10, QLatin1Char('0'));
        const ShareManager::PinOutcome out = mgr->redeemPin(token, wrong, "203.0.113.9");
        if (out.result == ShareManager::PinResult::RateLimited) sawLockout = true;
    }
    CHECK(sawLockout);

    // And the lockout answers before the PIN is even looked at, so a correct
    // PIN does not slip through the tier.
    const ShareManager::PinOutcome blocked = mgr->redeemPin(token, pin, "203.0.113.9");
    CHECK(blocked.result == ShareManager::PinResult::RateLimited);

    delete mgr;
}

void test_access_level_reads_both_flags()
{
    SECTION("share: the access badge is a table over both flags");
    ShareManager::Permissions p;
    CHECK_EQ(p.accessLevel(), QStringLiteral("viewer"));
    p.gamepad = true;
    CHECK_EQ(p.accessLevel(), QStringLiteral("gamer"));
    // Keyboard and mouse without a gamepad is its own thing. Reporting it as
    // "full" promised the owner a gamepad they had not granted.
    p.gamepad = false;
    p.keyboardMouse = true;
    CHECK_EQ(p.accessLevel(), QStringLiteral("desktop"));
    p.gamepad = true;
    CHECK_EQ(p.accessLevel(), QStringLiteral("full"));
}

void test_permissions_follow_the_board_live()
{
    SECTION("share: permissions move at any time, and the worker is told");
    ShareManager* mgr = freshManager();

    QVector<QPair<bool, bool>> pushed;
    QObject::connect(mgr, &ShareManager::permissionsChanged,
                     [&pushed](int, bool gamepad, bool km) { pushed.append({gamepad, km}); });

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    // A fresh share starts as viewer — the safe default, not the last one.
    CHECK(!st.permissions.gamepad);
    CHECK(!st.permissions.keyboardMouse);

    ShareManager::Permissions gamer;
    gamer.gamepad = true;
    CHECK(mgr->setPermissions(kSlot, gamer));
    CHECK_EQ(mgr->permissions(kSlot).accessLevel(), QStringLiteral("gamer"));
    CHECK_EQ(pushed.size(), 1);

    // Nothing freezes on a join any more: the guest streaming is precisely when
    // the owner is most likely to want to hand them the keyboard — or take it
    // back — and neither should cost them their picture.
    mgr->setStreaming(kSlot, true);
    ShareManager::Permissions full;
    full.gamepad = true;
    full.keyboardMouse = true;
    CHECK(mgr->setPermissions(kSlot, full));
    CHECK_EQ(mgr->permissions(kSlot).accessLevel(), QStringLiteral("full"));
    CHECK_EQ(pushed.size(), 2);

    // Demotion travels the same way, and is what releases the held keys.
    CHECK(mgr->setPermissions(kSlot, ShareManager::Permissions{}));
    CHECK_EQ(mgr->permissions(kSlot).accessLevel(), QStringLiteral("viewer"));
    CHECK_EQ(pushed.size(), 3);

    // Setting the same thing twice is not a change: a worker woken for nothing
    // would release the guest's held keys for nothing.
    CHECK(mgr->setPermissions(kSlot, ShareManager::Permissions{}));
    CHECK_EQ(pushed.size(), 3);

    // An Off slot takes the choice too: it is what the next START will be
    // minted with. Refusing it here is what let the board's poll repaint a tick
    // the owner had just made — the server had never been told.
    const int idle = ShareManager::kFirstSlot + 1;
    CHECK(mgr->setPermissions(idle, full));
    CHECK_EQ(mgr->permissions(idle).accessLevel(), QStringLiteral("full"));
    // No worker exists on an idle slot, so nothing was pushed anywhere.
    CHECK_EQ(pushed.size(), 3);

    QString idleToken, idlePin;
    ShareManager::SlotStatus idleStatus;
    mgr->activate(idle, kHost, kApp, kTtl, idleToken, idlePin, idleStatus);
    CHECK_EQ(idleStatus.permissions.accessLevel(), QStringLiteral("full"));

    // The next share remembers the last choice as its starting point.
    mgr->setStreaming(kSlot, false);
    CHECK(mgr->setPermissions(kSlot, gamer));
    QString token2, pin2;
    ShareManager::SlotStatus st2;
    mgr->activate(kSlot, kHost, kApp, kTtl, token2, pin2, st2);
    CHECK(st2.permissions.gamepad);
    CHECK(!st2.permissions.keyboardMouse);

    delete mgr;
}

void test_one_device_per_invitation()
{
    SECTION("share: a forwarded link buys no second seat, and shows up");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);
    CHECK(mgr->state(kSlot) == ShareManager::State::Shared);

    // Nobody has spent the PIN yet, so the join page still has a PIN to ask for.
    CHECK(!mgr->isBound(kSlot));

    const ShareManager::PinOutcome first =
        mgr->redeemPin(token, pin, "192.0.2.10", QStringLiteral("Mozilla/5.0 Chrome Windows"));
    CHECK(first.result == ShareManager::PinResult::Ok);
    // From here every correct PIN is answered AlreadyBound, so the join page
    // stops offering the form and shows the dead-link screen instead.
    CHECK(mgr->isBound(kSlot));
    // Spending the PIN is what makes a row Binded — not streaming.
    CHECK(mgr->state(kSlot) == ShareManager::State::Binded);
    CHECK(!mgr->isStreaming(kSlot));

    // The same PIN, elsewhere. Right secret, wrong device: refused, and not
    // counted as a wrong PIN — ten of these must not destroy the invitation the
    // legitimate guest is using.
    for (int i = 0; i < ShareManager::kMaxPinFailures + 2; ++i) {
        const ShareManager::PinOutcome again =
            mgr->redeemPin(token, pin, QStringLiteral("192.0.2.%1").arg(20 + i),
                           QStringLiteral("Mozilla/5.0 Firefox Linux"));
        CHECK(again.result == ShareManager::PinResult::AlreadyBound);
        CHECK(again.cookie.isEmpty());
    }
    CHECK(mgr->tokenIsLive(token));
    CHECK_EQ(mgr->slotForCookie(first.cookie), kSlot);

    // And the owner can see it happened — the thing that was invisible before.
    const ShareManager::SlotStatus row = mgr->status().at(kSlot - ShareManager::kFirstSlot);
    CHECK_EQ(row.devices.size(), 1);
    CHECK(row.devices.at(0).userAgent.contains(QStringLiteral("Chrome")));
    CHECK(row.lastRefusedAt > 0);
    CHECK(row.lastRefusedAgent.contains(QStringLiteral("Firefox")));

    // Re-opening the row clears the binding: that is how a guest who changed
    // browser gets back in.
    QString token2, pin2;
    mgr->activate(kSlot, kHost, kApp, kTtl, token2, pin2, st);
    CHECK(mgr->state(kSlot) == ShareManager::State::Shared);
    CHECK_EQ(mgr->slotForCookie(first.cookie), -1);
    CHECK(!mgr->isBound(kSlot));
    const ShareManager::PinOutcome fresh = mgr->redeemPin(token2, pin2, "192.0.2.99");
    CHECK(fresh.result == ShareManager::PinResult::Ok);

    delete mgr;
}

void test_lifetime_is_per_player()
{
    SECTION("share: each invitation carries its own lifetime");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;

    // Only the offered lifetimes. Anything else arrives from a browser.
    CHECK(!mgr->activate(kSlot, kHost, kApp, 1234, token, pin, st));
    CHECK(!ShareManager::isValidTtl(7200));
    CHECK(ShareManager::isValidTtl(3600));
    CHECK(ShareManager::isValidTtl(48 * 3600));
    CHECK(ShareManager::isValidTtl(0));

    CHECK(mgr->activate(kSlot, kHost, kApp, 24 * 3600, token, pin, st));
    CHECK_EQ(st.ttlSecs, qint64(24 * 3600));
    CHECK(st.expiresAt > 0);

    const qint64 now = QDateTime::currentSecsSinceEpoch();

    // Counted from now, so a fresh 24 h invitation set to 8 h has 8 h left.
    CHECK(mgr->setTtl(kSlot, 8 * 3600));
    CHECK(qAbs(mgr->expiresAt(kSlot) - (now + 8 * 3600)) <= 2);

    // And it only ever shortens. Asking for 24 h back does not buy the link a
    // day it never had; measuring from the opening instead would make the same
    // gesture mean "expire now" on an old invitation and "add hours" on a new
    // one.
    CHECK(mgr->setTtl(kSlot, 24 * 3600));
    CHECK(qAbs(mgr->expiresAt(kSlot) - (now + 8 * 3600)) <= 2);
    CHECK_EQ(mgr->ttlSecs(kSlot), qint64(24 * 3600)); // the slider still moved

    // Unlimited on a dated invitation is an extension, so it is refused the
    // same way. It stays reachable at START, which is where it belongs.
    CHECK(mgr->setTtl(kSlot, 0));
    CHECK(qAbs(mgr->expiresAt(kSlot) - (now + 8 * 3600)) <= 2);

    CHECK(!mgr->setTtl(kSlot, 99));

    // An invitation opened unlimited has no deadline for the sweep to invent,
    // and can still be cut short afterwards.
    QString t2, p2;
    CHECK(mgr->activate(kSlot, kHost, kApp, 0, t2, p2, st));
    CHECK_EQ(mgr->expiresAt(kSlot), qint64(0));
    CHECK(mgr->tokenIsLive(t2));
    CHECK(mgr->setTtl(kSlot, 4 * 3600));
    CHECK(qAbs(mgr->expiresAt(kSlot) - (now + 4 * 3600)) <= 2);
    delete mgr;

    // The deadline survives a restart, not just the lifetime that produced it.
    auto* restarted = new ShareManager();
    CHECK(qAbs(restarted->expiresAt(kSlot) - (now + 4 * 3600)) <= 2);
    CHECK_EQ(restarted->ttlSecs(kSlot), qint64(4 * 3600));
    restarted->deactivateAll(ShareManager::EndReason::OwnerStop);
    delete restarted;
}

void test_rows_can_be_named()
{
    SECTION("share: the board's rows carry the owner's own labels");
    ShareManager* mgr = freshManager();

    CHECK(mgr->name(kSlot).isEmpty()); // empty means "Player N"
    CHECK(mgr->setName(kSlot, QStringLiteral("  Léo  ")));
    CHECK_EQ(mgr->name(kSlot), QStringLiteral("Léo"));

    // Long enough for a name, and no longer.
    mgr->setName(kSlot, QString(200, QLatin1Char('x')));
    CHECK_EQ(mgr->name(kSlot).size(), ShareManager::kMaxNameLength);

    // Control characters never reach the board's markup.
    mgr->setName(kSlot, QStringLiteral("a\nb\tc"));
    CHECK_EQ(mgr->name(kSlot), QStringLiteral("a b c"));

    mgr->setName(kSlot, QStringLiteral("Marie"));
    CHECK(!mgr->setName(0, QStringLiteral("nope"))); // the owner slots are not ours
    delete mgr;

    // Names are settings, not credentials: they outlive both restarts and the
    // invitations they were typed next to.
    auto* restarted = new ShareManager();
    CHECK_EQ(restarted->name(kSlot), QStringLiteral("Marie"));
    delete restarted;
}

void test_a_dropped_stream_does_not_end_the_share()
{
    SECTION("share: only an owner action ends an invitation");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    mgr->setStreaming(kSlot, true);
    CHECK(mgr->isStreaming(kSlot));
    CHECK_EQ(mgr->streamingCount(), 1);

    // The player's connection drops (tab closed, ICE dead, host hiccup).
    mgr->setStreaming(kSlot, false);
    CHECK(!mgr->isStreaming(kSlot));
    CHECK_EQ(mgr->streamingCount(), 0);
    CHECK(mgr->tokenIsLive(token));

    // The owner clicking the row is what ends it.
    CHECK(mgr->deactivate(kSlot, ShareManager::EndReason::OwnerToggle));
    CHECK(mgr->state(kSlot) == ShareManager::State::Off);
    CHECK(!mgr->tokenIsLive(token));
    CHECK(!mgr->deactivate(kSlot, ShareManager::EndReason::OwnerToggle));

    delete mgr;
}

void test_revoking_a_live_stream_asks_for_a_disconnect()
{
    SECTION("share: revoking a streaming slot tells the stream to end");
    ShareManager* mgr = freshManager();

    QVector<QPair<int, int>> disconnects;
    QObject::connect(mgr, &ShareManager::playerMustDisconnect,
                     [&disconnects](int slot, int reason) { disconnects.append({slot, reason}); });

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    // Nothing streaming → nothing to disconnect.
    mgr->deactivate(kSlot, ShareManager::EndReason::OwnerToggle);
    CHECK_EQ(disconnects.size(), 0);

    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);
    mgr->setStreaming(kSlot, true);
    mgr->deactivate(kSlot, ShareManager::EndReason::OwnerStop);
    CHECK_EQ(disconnects.size(), 1);
    if (disconnects.size() == 1) {
        CHECK_EQ(disconnects.at(0).first, kSlot);
        CHECK_EQ(disconnects.at(0).second, static_cast<int>(ShareManager::EndReason::OwnerStop));
    }

    delete mgr;
}

void test_slots_are_independent()
{
    SECTION("share: one player's link never opens another's slot");
    ShareManager* mgr = freshManager();

    QString tokenA, pinA, tokenB, pinB;
    ShareManager::SlotStatus st;
    mgr->activate(ShareManager::kFirstSlot, kHost, kApp, kTtl, tokenA, pinA, st);
    mgr->activate(ShareManager::kFirstSlot + 1, kHost, kApp, kTtl, tokenB, pinB, st);

    // The right link with the wrong PIN is still refused.
    const ShareManager::PinOutcome crossed = mgr->redeemPin(tokenA, pinB, "198.51.100.1");
    CHECK(crossed.result == ShareManager::PinResult::Invalid);

    const ShareManager::PinOutcome a = mgr->redeemPin(tokenA, pinA, "198.51.100.2");
    const ShareManager::PinOutcome b = mgr->redeemPin(tokenB, pinB, "198.51.100.3");
    CHECK_EQ(a.slot, ShareManager::kFirstSlot);
    CHECK_EQ(b.slot, ShareManager::kFirstSlot + 1);
    CHECK_EQ(mgr->slotForCookie(a.cookie), ShareManager::kFirstSlot);
    CHECK_EQ(mgr->slotForCookie(b.cookie), ShareManager::kFirstSlot + 1);

    // The link, not the cookie, is what names the player: the routes resolve
    // the slot from the token and then demand that the cookie agree. Without
    // that, a browser still holding A's cookie would be read as A when it
    // opens B's link — and shown A's state.
    CHECK_EQ(mgr->slotForToken(tokenA), ShareManager::kFirstSlot);
    CHECK_EQ(mgr->slotForToken(tokenB), ShareManager::kFirstSlot + 1);
    CHECK(mgr->slotForCookie(a.cookie) != mgr->slotForToken(tokenB));
    CHECK_EQ(mgr->slotForToken(QStringLiteral("not-a-token")), -1);

    // Ending everything ends everything.
    mgr->deactivateAll(ShareManager::EndReason::OwnerStop);
    CHECK_EQ(mgr->slotForCookie(a.cookie), -1);
    CHECK_EQ(mgr->slotForCookie(b.cookie), -1);

    delete mgr;
}

void test_persistence_survives_a_restart()
{
    SECTION("share: a live invitation outlives a restart");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);
    ShareManager::Permissions p;
    p.gamepad = true;
    mgr->setPermissions(kSlot, p);
    const ShareManager::PinOutcome ok = mgr->redeemPin(token, pin, "192.0.2.7");
    CHECK(ok.result == ShareManager::PinResult::Ok);
    mgr->setStreaming(kSlot, true);
    delete mgr;

    // A restart is not an owner action, so the link keeps working — but the
    // worker died with the process, so nothing is streaming any more.
    auto* restarted = new ShareManager();
    CHECK(restarted->tokenIsLive(token));
    // Still Binded: the device that spent the PIN is remembered, so the guest
    // comes back without being asked for it again.
    CHECK(restarted->state(kSlot) == ShareManager::State::Binded);
    CHECK_EQ(restarted->streamingCount(), 0);
    CHECK_EQ(restarted->slotForCookie(ok.cookie), kSlot);
    CHECK(restarted->permissions(kSlot).gamepad);

    restarted->deactivateAll(ShareManager::EndReason::OwnerStop);
    delete restarted;

    // Revoked before the restart stays revoked after it.
    auto* again = new ShareManager();
    CHECK(!again->tokenIsLive(token));
    delete again;
}

void test_clear_secrets_are_memory_only()
{
    SECTION("share: the clear link and PIN never touch the disk");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st);

    // The owner reopening the popin gets the same pair back, not a new one.
    QString again, againPin;
    CHECK(mgr->secrets(kSlot, again, againPin));
    CHECK_EQ(again, token);
    CHECK_EQ(againPin, pin);

    // Another slot has nothing to show.
    QString other, otherPin;
    CHECK(!mgr->secrets(ShareManager::kFirstSlot + 1, other, otherPin));
    delete mgr;

    // The invitation itself survives a restart — its clear pair does not,
    // because share.json only ever held the digests.
    auto* restarted = new ShareManager();
    CHECK(restarted->tokenIsLive(token));
    QString lostToken, lostPin;
    CHECK(!restarted->secrets(kSlot, lostToken, lostPin));
    CHECK(lostToken.isEmpty());
    CHECK(lostPin.isEmpty());

    // Revoking wipes them for good.
    restarted->deactivateAll(ShareManager::EndReason::OwnerStop);
    CHECK(!restarted->secrets(kSlot, lostToken, lostPin));
    delete restarted;
}

void test_share_is_bound_to_its_host()
{
    SECTION("share: an activation is bound to one host, and only that host");
    ShareManager* mgr = freshManager();

    // Off slots name no host.
    CHECK(mgr->hostForSlot(kSlot).isEmpty());
    CHECK_EQ(mgr->appForSlot(kSlot), -1);

    QString token, pin;
    ShareManager::SlotStatus st;
    CHECK(mgr->activate(kSlot, kHost, kApp, kTtl, token, pin, st));
    CHECK_EQ(mgr->hostForSlot(kSlot), kHost);
    CHECK_EQ(mgr->appForSlot(kSlot), kApp);

    // Re-sharing rebinds to whatever host is up at that moment.
    QString token2, pin2;
    CHECK(mgr->activate(kSlot, QStringLiteral("host-uuid-B"), 7, kTtl, token2, pin2, st));
    CHECK_EQ(mgr->hostForSlot(kSlot), QStringLiteral("host-uuid-B"));
    CHECK_EQ(mgr->appForSlot(kSlot), 7);
    delete mgr;

    // The binding outlives a restart — a link handed out on host B must still
    // resolve to host B, never to whatever is up next.
    auto* restarted = new ShareManager();
    CHECK_EQ(restarted->hostForSlot(kSlot), QStringLiteral("host-uuid-B"));
    CHECK_EQ(restarted->appForSlot(kSlot), 7);

    // Revoking drops the binding with everything else.
    restarted->deactivate(kSlot, ShareManager::EndReason::OwnerToggle);
    CHECK(restarted->hostForSlot(kSlot).isEmpty());
    CHECK_EQ(restarted->appForSlot(kSlot), -1);
    delete restarted;
}

} // namespace

void run_share_manager_tests()
{
    test_activation_mints_distinct_secrets();
    test_pin_is_required_and_bound_to_the_link();
    test_pin_bruteforce_kills_the_activation();
    test_rate_limit_locks_a_single_caller();
    test_access_level_reads_both_flags();
    test_permissions_follow_the_board_live();
    test_one_device_per_invitation();
    test_lifetime_is_per_player();
    test_rows_can_be_named();
    test_a_dropped_stream_does_not_end_the_share();
    test_revoking_a_live_stream_asks_for_a_disconnect();
    test_slots_are_independent();
    test_persistence_survives_a_restart();
    test_clear_secrets_are_memory_only();
    test_share_is_bound_to_its_host();
}
