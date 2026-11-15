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

#pragma once

#include "server/StreamOwner.h"

#include <QList>
#include <QString>

/**
 * Whether a request may end, replace or join someone else's stream in a booth
 * whose streams have PINs (booth_stream_pin, issue #68).
 *
 * Pure: the caller says who asks, what for, and whose streams the request
 * would reach on that host; this says yes, yes because nobody used the stream
 * for a while, only with the stream's PIN, or no. Nothing here starts, ends or
 * looks anything up.
 *
 * What a request is let through on is what it answers for: a take-over ends
 * every stream of the others in reach (Decision::othersSlots), not only the
 * ones a start would have ended anyway.
 *
 * In this order, the first that applies decides:
 *  - the key off: as without it;
 *  - nobody's stream in reach, or only the caller's own: as without it;
 *  - a second leg or a start again after a crash, from anyone else: no;
 *  - this machine: yes, whatever is in reach;
 *  - a caller nobody can name (no session, not this machine): no;
 *  - a quit of someone else's stream: no;
 *  - a stream nobody can be told apart from (an Unknown owner): no;
 *  - streams of more than one other person: no, no one PIN opens them all;
 *  - the stream unused for the idle time: yes, and its owner is told;
 *  - otherwise: only with that stream's PIN.
 */
namespace BoothGate {

/// What the request does to the host's streams.
enum class Action
{
    Fresh,       ///< a start that is nobody's second leg: the same app or another
    Fallback,    ///< the same start again on the next transport (transport_index > 0)
    Standby,     ///< the second leg of a quality switch (standby + session_slot)
    Reconnect,   ///< a start again after a worker crash (reconnect_of)
    StopSession, ///< POST stop-session: every stream of the host
    Quit,        ///< POST quit: the streams it names
};

enum class Verdict
{
    Allow,     ///< go ahead, as without the key
    AllowIdle, ///< someone else's stream, unused for the idle time: go ahead, they are told
    NeedPin,   ///< someone else's stream: only with its PIN
    Refuse,    ///< someone else's stream, and no PIN opens this
};

/// Why, for the tests and the logs (never anybody's token).
enum class Why
{
    KeyOff,
    NothingInReach,
    SameOwner,
    ThisMachine,
    Idle,
    PinRequired,
    OwnerOnly,
    ReconnectRefused,
    NobodyKnown,
    SeveralOwners,
};

/// A stream the request would reach: whose it is, the key its PIN is kept
/// under, and the slot it streams in.
struct Stream
{
    StreamOwner owner;
    QString key;
    /// -1 for one in no slot: a start not up yet, the stream this process runs itself.
    int slot = -1;
};

struct Input
{
    /// booth_stream_pin, in a booth (HttpRequest::boothPin).
    bool keyOn = false;
    /// Who asks (StreamOwner::of(req.isThisMachine, token)).
    StreamOwner requester;
    Action action = Action::Fresh;
    /// The streams the request would end, replace or share on that host,
    /// one entry per slot (a standby leg is its stream again).
    QList<Stream> inReach;
    qint64 nowMs = 0;
    /// The last input on the streams of others in reach, their guests'
    /// included, or their start when nobody has touched them yet. 0 or less:
    /// not known, which is never idle.
    qint64 lastActivityMs = 0;
    /// booth_takeover_idle_minutes; 0: a stream is never idle.
    int idleMinutes = 0;
};

struct Decision
{
    Verdict verdict = Verdict::Refuse;
    Why why = Why::OwnerOnly;
    /// NeedPin and AllowIdle: the key of the stream taken over, whose PIN it is.
    QString streamKey;
    /// The slots of the others' streams in reach, each once, in order. A
    /// request let through on them ends them all, the taken-over owner told.
    QList<int> othersSlots;

    bool allowed() const { return verdict == Verdict::Allow || verdict == Verdict::AllowIdle; }
    /// The error on the wire, empty when allowed.
    QString code() const;
    /// The status it goes with, 200 when allowed.
    int status(Action action) const;
};

/// Whether a quit reaches the stream a page of @p slotUniqueId started. A quit
/// that ends the host's app for everyone (@p endsTheApp: a host-wide cancel)
/// reaches every stream there, whatever the page names; any other reaches the
/// streams of the browser it names, an empty id on either side read as that
/// browser's (as the quit itself picks them).
bool quitReaches(bool endsTheApp, const QString& quitUniqueId, const QString& slotUniqueId);

/// Whether a stream last used at @p lastActivityMs is idle at @p nowMs.
bool idle(qint64 nowMs, qint64 lastActivityMs, int idleMinutes);

Decision decide(const Input& in);

/// The verdict's name, for logs.
const char* whyName(Why why);

} // namespace BoothGate
