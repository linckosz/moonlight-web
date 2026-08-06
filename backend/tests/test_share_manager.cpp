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

#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QVector>

namespace {

const int kSlot = ShareManager::kFirstSlot;

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
    CHECK(mgr->activate(kSlot, token1, pin1, st1));
    CHECK(st1.state == ShareManager::State::Shared);
    // 32 random bytes, base64url, unpadded.
    CHECK(token1.size() >= 40);
    CHECK_EQ(pin1.size(), 6);
    CHECK(pin1.toInt() >= 0);

    QString token2, pin2;
    ShareManager::SlotStatus st2;
    CHECK(mgr->activate(kSlot, token2, pin2, st2));
    CHECK(token1 != token2);
    // Re-sharing kills the old link outright — that is the only way to change
    // the permissions, so it must not leave the previous one usable.
    CHECK(!mgr->tokenIsLive(token1));
    CHECK(mgr->tokenIsLive(token2));

    // A slot outside the player range is not a slot.
    QString t3, p3;
    ShareManager::SlotStatus s3;
    CHECK(!mgr->activate(0, t3, p3, s3));
    CHECK(!mgr->activate(ShareManager::kLastSlot + 1, t3, p3, s3));

    delete mgr;
}

void test_pin_is_required_and_bound_to_the_link()
{
    SECTION("share: the PIN is the second factor, and it is per-activation");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, token, pin, st);

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
    mgr->activate(kSlot, token2, pin2, st);
    CHECK_EQ(mgr->slotForCookie(ok.cookie), -1);

    delete mgr;
}

void test_pin_bruteforce_kills_the_activation()
{
    SECTION("share: repeated wrong PINs destroy the invitation, never open it");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, token, pin, st);

    // Each attempt comes from its own bucket so the per-caller lockout never
    // fires — this is the attacker with a botnet, and the per-activation
    // counter is what stops them.
    for (int i = 0; i < ShareManager::kMaxPinFailures; ++i) {
        const QString wrong = QStringLiteral("%1").arg((pin.toInt() + i + 1) % 1000000, 6, 10,
                                                       QLatin1Char('0'));
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
    mgr->activate(kSlot, token, pin, st);

    bool sawLockout = false;
    for (int i = 0; i < 4; ++i) {
        const QString wrong = QStringLiteral("%1").arg((pin.toInt() + i + 1) % 1000000, 6, 10,
                                                       QLatin1Char('0'));
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

void test_permissions_freeze_when_the_popin_closes()
{
    SECTION("share: permissions are final once locked");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, token, pin, st);

    // A fresh share starts as viewer — the safe default, not the last one.
    CHECK(!st.permissions.gamepad);
    CHECK(!st.permissions.keyboardMouse);

    ShareManager::Permissions gamer;
    gamer.gamepad = true;
    CHECK(mgr->setPermissions(kSlot, gamer));
    CHECK_EQ(mgr->permissions(kSlot).accessLevel(), QStringLiteral("gamer"));

    CHECK(mgr->lockPermissions(kSlot));

    ShareManager::Permissions full;
    full.gamepad = true;
    full.keyboardMouse = true;
    // Refused: a worker may already be enforcing the old policy, and the player
    // was told what they were getting.
    CHECK(!mgr->setPermissions(kSlot, full));
    CHECK_EQ(mgr->permissions(kSlot).accessLevel(), QStringLiteral("gamer"));

    // The next share remembers the choice as its starting point.
    QString token2, pin2;
    ShareManager::SlotStatus st2;
    mgr->activate(kSlot, token2, pin2, st2);
    CHECK(st2.permissions.gamepad);
    CHECK(!st2.permissions.keyboardMouse);

    delete mgr;
}

void test_a_dropped_stream_does_not_end_the_share()
{
    SECTION("share: only an owner action ends an invitation");
    ShareManager* mgr = freshManager();

    QString token, pin;
    ShareManager::SlotStatus st;
    mgr->activate(kSlot, token, pin, st);

    mgr->setStreaming(kSlot, true);
    CHECK(mgr->state(kSlot) == ShareManager::State::Streaming);
    CHECK_EQ(mgr->streamingCount(), 1);
    // Joining freezes the permissions even if the popin is still open.
    ShareManager::Permissions p;
    p.keyboardMouse = true;
    CHECK(!mgr->setPermissions(kSlot, p));

    // The player's connection drops (tab closed, ICE dead, host hiccup).
    mgr->setStreaming(kSlot, false);
    CHECK(mgr->state(kSlot) == ShareManager::State::Shared);
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
    mgr->activate(kSlot, token, pin, st);

    // Nothing streaming → nothing to disconnect.
    mgr->deactivate(kSlot, ShareManager::EndReason::OwnerToggle);
    CHECK_EQ(disconnects.size(), 0);

    mgr->activate(kSlot, token, pin, st);
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
    mgr->activate(ShareManager::kFirstSlot, tokenA, pinA, st);
    mgr->activate(ShareManager::kFirstSlot + 1, tokenB, pinB, st);

    // The right link with the wrong PIN is still refused.
    const ShareManager::PinOutcome crossed = mgr->redeemPin(tokenA, pinB, "198.51.100.1");
    CHECK(crossed.result == ShareManager::PinResult::Invalid);

    const ShareManager::PinOutcome a = mgr->redeemPin(tokenA, pinA, "198.51.100.2");
    const ShareManager::PinOutcome b = mgr->redeemPin(tokenB, pinB, "198.51.100.3");
    CHECK_EQ(a.slot, ShareManager::kFirstSlot);
    CHECK_EQ(b.slot, ShareManager::kFirstSlot + 1);
    CHECK_EQ(mgr->slotForCookie(a.cookie), ShareManager::kFirstSlot);
    CHECK_EQ(mgr->slotForCookie(b.cookie), ShareManager::kFirstSlot + 1);

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
    mgr->activate(kSlot, token, pin, st);
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
    CHECK(restarted->state(kSlot) == ShareManager::State::Shared);
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
    mgr->activate(kSlot, token, pin, st);

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

} // namespace

void run_share_manager_tests()
{
    test_activation_mints_distinct_secrets();
    test_pin_is_required_and_bound_to_the_link();
    test_pin_bruteforce_kills_the_activation();
    test_rate_limit_locks_a_single_caller();
    test_permissions_freeze_when_the_popin_closes();
    test_a_dropped_stream_does_not_end_the_share();
    test_revoking_a_live_stream_asks_for_a_disconnect();
    test_slots_are_independent();
    test_persistence_survives_a_restart();
    test_clear_secrets_are_memory_only();
}
