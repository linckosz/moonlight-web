/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "server/BoothGate.h"

namespace BoothGate {

namespace {

Decision make(Verdict verdict, Why why, const QString& key = QString())
{
    Decision d;
    d.verdict = verdict;
    d.why = why;
    d.streamKey = key;
    return d;
}

} // namespace

bool idle(qint64 nowMs, qint64 lastActivityMs, int idleMinutes)
{
    // Off, or a last use nobody knows: never idle. A stream must have been
    // seen to go unused, not merely not seen.
    if (idleMinutes <= 0 || lastActivityMs <= 0) return false;
    return nowMs - lastActivityMs >= static_cast<qint64>(idleMinutes) * 60 * 1000;
}

bool quitReaches(bool endsTheApp, const QString& quitUniqueId, const QString& slotUniqueId)
{
    // The page's id is the page's word: it never keeps a host-wide cancel off
    // somebody's stream.
    if (endsTheApp) return true;
    return slotUniqueId.isEmpty() || quitUniqueId.isEmpty() || quitUniqueId == slotUniqueId;
}

namespace {

/// The verdict on @p others, the streams in reach that are not the caller's.
Decision verdictOn(const Input& in, const QList<Stream>& others)
{
    if (others.isEmpty())
        return make(Verdict::Allow, in.inReach.isEmpty() ? Why::NothingInReach : Why::SameOwner);

    // A second leg or a start again after a crash is only ever the owner's own
    // stream continuing: from anyone else it is not a newcomer to let in, and
    // no PIN makes it one.
    if (in.action == Action::Standby) return make(Verdict::Refuse, Why::OwnerOnly);
    if (in.action == Action::Reconnect) return make(Verdict::Refuse, Why::ReconnectRefused);

    // Someone at the PC itself can always take it back.
    if (in.requester.kind == StreamOwner::Kind::ThisMachine)
        return make(Verdict::Allow, Why::ThisMachine);
    // A caller nobody can name has no session to hold a PIN's tries by, and
    // is nobody to let in, idle stream or not.
    if (in.requester.kind == StreamOwner::Kind::Unknown)
        return make(Verdict::Refuse, Why::NobodyKnown);

    // Quitting is for one's own streams only: there is no PIN for it.
    if (in.action == Action::Quit) return make(Verdict::Refuse, Why::OwnerOnly);

    // A stream nobody can be told apart from has no owner to read its PIN,
    // nor to be told: only this machine ends it.
    const QString key = others.first().key;
    for (const Stream& s : others) {
        if (s.owner.kind == StreamOwner::Kind::Unknown || s.key.isEmpty())
            return make(Verdict::Refuse, Why::NobodyKnown);
        // One PIN opens one stream: the PIN of one person's would end another's.
        if (s.key != key) return make(Verdict::Refuse, Why::SeveralOwners);
    }

    if (idle(in.nowMs, in.lastActivityMs, in.idleMinutes))
        return make(Verdict::AllowIdle, Why::Idle, key);
    return make(Verdict::NeedPin, Why::PinRequired, key);
}

} // namespace

Decision decide(const Input& in)
{
    if (!in.keyOn) return make(Verdict::Allow, Why::KeyOff);

    // Whose streams are in reach, the caller's own left out.
    QList<Stream> others;
    for (const Stream& s : in.inReach)
        if (!s.owner.sameAs(in.requester)) others.append(s);
    Decision d = verdictOn(in, others);
    for (const Stream& s : others)
        if (s.slot >= 0 && !d.othersSlots.contains(s.slot)) d.othersSlots.append(s.slot);
    return d;
}

QString Decision::code() const
{
    switch (why) {
    case Why::KeyOff:
    case Why::NothingInReach:
    case Why::SameOwner:
    case Why::ThisMachine:
    case Why::Idle: return QString();
    case Why::PinRequired: return QStringLiteral("booth_pin_required");
    case Why::ReconnectRefused: return QStringLiteral("reconnect_refused");
    case Why::OwnerOnly:
    case Why::NobodyKnown:
    case Why::SeveralOwners: break;
    }
    return QStringLiteral("booth_owner_only");
}

int Decision::status(Action action) const
{
    if (allowed()) return 200;
    // A quit of someone else's stream is a request this caller may not make;
    // anything else is a stream in the way, which a 409 says.
    return action == Action::Quit ? 403 : 409;
}

const char* whyName(Why why)
{
    switch (why) {
    case Why::KeyOff: return "key-off";
    case Why::NothingInReach: return "nothing-in-reach";
    case Why::SameOwner: return "same-owner";
    case Why::ThisMachine: return "this-machine";
    case Why::Idle: return "idle";
    case Why::PinRequired: return "pin-required";
    case Why::OwnerOnly: return "owner-only";
    case Why::ReconnectRefused: return "reconnect-refused";
    case Why::NobodyKnown: return "nobody-known";
    case Why::SeveralOwners: return "several-owners";
    }
    return "unknown";
}

} // namespace BoothGate
