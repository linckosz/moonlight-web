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

#include "Tcp443Guard.h"

#include <QStringList>

#include <utility>

namespace mw::tcp443 {

using mw::routerholes::Hole;
using mw::routerholes::Kind;
using Lookup = IUpnpGateway::Lookup;

namespace {

const QString kTcp = QStringLiteral("TCP");

Verdict left(int step, const QString& said)
{
    return {Outcome::Left, step, said};
}

/// The run is over.
Step ends(const Verdict& verdict)
{
    Step step;
    step.verdict = verdict;
    return step;
}

/// The run waits for this dial.
Step asks(Dial::Kind kind, const QString& address, uint16_t port, uint16_t leadsHereOn)
{
    Step step;
    step.dial.kind = kind;
    step.dial.address = address;
    step.dial.port = port;
    step.dial.leadsHereOn = leadsHereOn;
    return step;
}

/// What is said of an entry the router keeps when it was to be taken off.
QString keptText(int step)
{
    return QStringLiteral("the entry could not be taken off the router (step %1): it stays there "
                          "until the next start takes it back, or until it is removed on the "
                          "router itself")
        .arg(step);
}

QString takenOffText()
{
    return QStringLiteral("what TCP 443 stood on changed: the entry is taken off");
}

} // namespace

bool publicIpv4(const QString& address)
{
    if (!mw::routerholes::dottedIpv4(address)) return false;
    const QStringList parts = address.split(QLatin1Char('.'));
    const int a = parts.at(0).toInt();
    const int b = parts.at(1).toInt();
    const int c = parts.at(2).toInt();
    if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
    if (a == 100 && b >= 64 && b <= 127) return false; // an operator's shared range
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 192 && b == 0 && (c == 0 || c == 2)) return false; // protocols, documentation
    if (a == 192 && b == 88 && c == 99) return false;           // a relay's
    if (a == 198 && (b == 18 || b == 19)) return false;         // benches
    if (a == 198 && b == 51 && c == 100) return false;          // documentation
    if (a == 203 && b == 0 && c == 113) return false;           // documentation
    return true;
}

Guard::Guard(IUpnpGateway& gateway, mw::routerholes::IRecord* record)
    : m_Gateway(gateway)
    , m_Record(record)
{}

QString Guard::text() const
{
    return mw::routerholes::describe(Kind::Tcp443, -1, m_Record ? m_Record->tag() : QString());
}

bool Guard::ours(const IUpnpGateway::Entry& shown, const Hole& line) const
{
    // By the record and by the text, and as it was noted: the address and the
    // port. Never by the address alone.
    return line.kind == Kind::Tcp443 && line.tcp && line.external == kPort && !line.lan.isEmpty() &&
           shown.client == line.lan && shown.internalPort == line.internal &&
           shown.description == text();
}

Step Guard::begin(const Situation& now)
{
    // One run at a time, and never an entry left by one that was forgotten
    abandon();

    if (!now.wanted) return ends(left(0, QStringLiteral("not asked for")));
    if (!m_Record)
        return ends(
            left(0, QStringLiteral("this run keeps no record of what it writes on the router")));
    if (now.blockPort == 0)
        return ends(
            left(0, QStringLiteral("the router forwards no shared TCP port for it to lead to")));

    // 1. An address worth it
    if (!publicIpv4(now.publicAddress) || now.routerAddress != now.publicAddress)
        return ends(left(1, QStringLiteral("the router's own address is not a public one browsers "
                                           "reach this host at")));

    // An entry of this installation's under an address this machine no longer
    // has is not somebody's 443: it is taken off first, when the router lets it
    const Former former = formerAddress();
    if (former == Former::Refused)
        return ends(left(2, QStringLiteral("the router keeps a TCP 443 entry this installation "
                                           "wrote under an address it no longer has")));
    if (former == Former::Unknown)
        return ends(
            left(2, QStringLiteral("the router does not say whether TCP 443 is forwarded")));

    // 2. Nobody has 443
    const QString lan = m_Gateway.lanAddress();
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Unknown)
        return ends(
            left(2, QStringLiteral("the router does not say whether TCP 443 is forwarded")));
    Hole noted;
    const bool hasLine = m_Record->find(kPort, true, noted) && noted.lan == lan;
    if (found == Lookup::Found) {
        if (!hasLine || !ours(shown, noted))
            return ends(left(2, QStringLiteral("the router already forwards TCP 443")));
        const bool unprovedLine = unproved(noted);
        if (noted.internal == now.blockPort && !unprovedLine) {
            // This installation's, and where it should lead: kept (step 6)
            m_Held = true;
            m_Internal = noted.internal;
            return ends(renew(now));
        }
        // This installation's, towards a port the streams no longer share;
        // or written by this run of the server and never proved, which no
        // dial has shown to lead here: never taken as held. Taken off, and
        // asked for anew, as a free 443 is
        if (!m_Gateway.remove(kPort, kTcp) || m_Gateway.entry(kPort, kTcp, shown) != Lookup::None) {
            if (!unprovedLine)
                return ends(left(2, QStringLiteral("the router keeps this installation's TCP 443 "
                                                   "entry towards a port it left")));
            // Kept again: tried again in an hour, not at every look
            Step kept = ends(
                left(2, QStringLiteral("the router keeps an entry this host wrote and could not "
                                       "prove")));
            kept.apart = true;
            return kept;
        }
        strike();
    } else if (hasLine) {
        // Noted, and the router has it no more
        strike();
    }

    // 3. The router leads back here: that is a dial, and the caller's to have
    // made. The run waits for it, on what it was begun on.
    m_Run = now;
    m_Stage = Stage::WayBack;
    return asks(Dial::Kind::ArrivesHere, now.publicAddress, now.blockPort, now.blockPort);
}

Step Guard::after(bool result)
{
    const Stage stage = m_Stage;
    m_Stage = Stage::Idle;
    switch (stage) {
    case Stage::Idle: break;
    case Stage::WayBack:
        // 3. The router leads back here
        if (!result)
            return ends(left(3, QStringLiteral("the router does not lead back here from its own "
                                               "address: 443 cannot be checked from this host")));
        // 4. 443 answers nobody today: the next dial
        m_Stage = Stage::Silent;
        return asks(Dial::Kind::Answers, m_Run.publicAddress, kPort, 0);
    case Stage::Silent:
        if (result) return ends(left(4, QStringLiteral("something answers on TCP 443 already")));
        return write();
    case Stage::Proof:
        // 5, its end: what was written is proved, or taken back
        if (!result) {
            undo(m_Line);
            return ends(left(5, QStringLiteral("a connection to TCP 443 does not arrive here")));
        }
        m_Held = true;
        m_Internal = m_Line.internal;
        return ends({Outcome::Held, 5, QStringLiteral("TCP 443 leads to the shared port")});
    }
    // No dial was asked for: nothing to go on from, and the router is left alone
    return ends(left(0, QStringLiteral("no run is under way")));
}

Step Guard::write()
{
    // 5. Write, read, prove. The two dials took their time, and this thread
    // served other requests meanwhile: what step 2 asked is asked again, now,
    // just before the write. The record has no line of this installation's on
    // 443 at this point (step 2 saw to that), so an entry is somebody's.
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Unknown)
        return ends(
            left(2, QStringLiteral("the router does not say whether TCP 443 is forwarded")));
    if (found == Lookup::Found)
        return ends(left(2, QStringLiteral("the router already forwards TCP 443")));

    // Noted first: an entry the router has and the record has not could never
    // be taken back.
    Hole line;
    line.external = kPort;
    line.tcp = true;
    line.internal = m_Run.blockPort;
    line.kind = Kind::Tcp443;
    line.slot = -1;
    line.lan = m_Gateway.lanAddress();
    m_Record->note(line);
    // The record's one line on 443 is this one from here on
    m_HasUnproved = false;
    if (!m_Gateway.add(kPort, m_Run.blockPort, m_Run.leaseSec, text(), kTcp)) {
        undo(line);
        return ends(left(5, QStringLiteral("the router refused the entry")));
    }
    if (m_Gateway.entry(kPort, kTcp, shown) != Lookup::Found || !ours(shown, line)) {
        undo(line);
        return ends(
            left(5, QStringLiteral("the router does not show the entry as it was asked for")));
    }
    // Written and read back: a connection to 443 must now arrive here
    m_Line = line;
    m_Stage = Stage::Proof;
    return asks(Dial::Kind::ArrivesHere, m_Run.publicAddress, kPort, m_Run.blockPort);
}

Guard::Abandoned Guard::abandon()
{
    const Stage stage = m_Stage;
    m_Stage = Stage::Idle;
    // Before the write there is nothing to take back
    if (stage != Stage::Proof) return Abandoned::Nothing;
    return undo(m_Line) ? Abandoned::Undone : Abandoned::Stays;
}

Verdict Guard::claim(const Situation& now, IProbe& probe)
{
    Step step = begin(now);
    while (!step.over())
        step = after(probe.make(step.dial));
    return step.verdict;
}

Verdict Guard::renew(const Situation& now)
{
    if (!m_Held || !m_Record) return left(6, QStringLiteral("not held"));

    // What it stood on is no more: taken off
    if (!now.wanted || now.blockPort != m_Internal || !publicIpv4(now.publicAddress) ||
        now.routerAddress != now.publicAddress) {
        release();
        return {Outcome::Lost, 6,
                QStringLiteral("what TCP 443 stood on changed: the entry is taken off")};
    }

    const QString lan = m_Gateway.lanAddress();
    Hole line;
    const bool noted = m_Record->find(kPort, true, line) && line.lan == lan &&
                       line.internal == m_Internal && line.kind == Kind::Tcp443;
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (!noted || found != Lookup::Found || !ours(shown, line)) {
        m_Held = false;
        // Gone, or another's: no longer this installation's, and the line
        // goes. Not shown at all: it may still be there, and the line stays.
        if (noted && found != Lookup::Unknown) strike();
        return {Outcome::Lost, 6,
                found == Lookup::Unknown
                    ? QStringLiteral("the router no longer says what TCP 443 leads to")
                    : QStringLiteral("TCP 443 is no longer this host's on the router")};
    }

    // Written again for its lease, and read back
    if (!m_Gateway.add(kPort, m_Internal, now.leaseSec, text(), kTcp) ||
        m_Gateway.entry(kPort, kTcp, shown) != Lookup::Found || !ours(shown, line)) {
        m_Held = false;
        return {Outcome::Lost, 6, QStringLiteral("the router did not keep the TCP 443 entry")};
    }
    return {Outcome::Held, 6, QStringLiteral("TCP 443 still leads to the shared port")};
}

bool Guard::release()
{
    m_Held = false;
    // A run under way is over: what it wrote is in the record, and is taken
    // off below as what was held is
    m_Stage = Stage::Idle;
    if (!m_Record) return true;
    Hole line;
    if (!m_Record->find(kPort, true, line) || line.kind != Kind::Tcp443) return true;
    // Written under another address: its own rule
    if (line.lan != m_Gateway.lanAddress()) {
        const Former former = formerAddress();
        return former != Former::Refused && former != Former::Unknown;
    }
    IUpnpGateway::Entry shown;
    Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Found && ours(shown, line)) {
        m_Gateway.remove(kPort, kTcp);
        found = m_Gateway.entry(kPort, kTcp, shown);
    }
    // Gone, or another's by now: nothing of this installation's is there
    if (found == Lookup::None || (found == Lookup::Found && !ours(shown, line))) {
        strike();
        return true;
    }
    return false;
}

bool Guard::undo(const Hole& line)
{
    IUpnpGateway::Entry shown;
    Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    if (found == Lookup::Found && ours(shown, line)) {
        m_Gateway.remove(kPort, kTcp);
        found = m_Gateway.entry(kPort, kTcp, shown);
    }
    // Read back: gone, or not this installation's (the router wrote something
    // else than it was asked). Still shown as written, or not shown at all:
    // the line stays, and the next start takes the entry back. Until then it
    // is the line written and never proved: no run takes it as held.
    if (found == Lookup::None || (found == Lookup::Found && !ours(shown, line))) {
        strike();
        return true;
    }
    m_Unproved = line;
    m_HasUnproved = true;
    return false;
}

bool Guard::unproved(const Hole& line) const
{
    return m_HasUnproved && line.kind == m_Unproved.kind && line.external == m_Unproved.external &&
           line.tcp == m_Unproved.tcp && line.internal == m_Unproved.internal &&
           line.lan == m_Unproved.lan;
}

void Guard::strike()
{
    m_Record->strike(kPort, true);
    m_HasUnproved = false;
}

Guard::Former Guard::formerAddress()
{
    Hole line;
    if (!m_Record || !m_Record->find(kPort, true, line) || line.kind != Kind::Tcp443 ||
        line.lan == m_Gateway.lanAddress()) {
        m_FormerRefused = false;
        return Former::None;
    }
    IUpnpGateway::Entry shown;
    const Lookup found = m_Gateway.entry(kPort, kTcp, shown);
    // The router says nothing: the entry may be there still, and what was
    // said of it stands
    if (found == Lookup::Unknown) return Former::Unknown;
    if (found == Lookup::None || !ours(shown, line)) {
        strike();
        m_FormerRefused = false;
        return Former::Gone;
    }
    // Still as this installation wrote it, towards an address that may be
    // another machine's by now
    if (m_Gateway.remove(kPort, kTcp) && m_Gateway.entry(kPort, kTcp, shown) == Lookup::None) {
        strike();
        m_FormerRefused = false;
        return Former::Removed;
    }
    m_FormerRefused = true;
    return Former::Refused;
}

void tellAdmin(QJsonObject& status, bool admin, bool staleRemovalRefused)
{
    const QLatin1String key(kStaleRefusedKey);
    if (admin)
        status[key] = staleRemovalRefused;
    else
        status.remove(key);
}

void Pace::said(const Verdict& verdict, uint64_t nowMs, bool dialled) noexcept
{
    switch (verdict.outcome) {
    case Outcome::Held: m_Held = true; return;
    case Outcome::Lost:
        m_Held = false;
        // At once, once: a router that was restarted has forgotten the entry.
        // Lost again within the hour, it is somebody's doing, and waits.
        m_NotBeforeMs = m_LostBefore && nowMs - m_LostMs < kApartMs ? nowMs + kApartMs : nowMs;
        m_LostBefore = true;
        m_LostMs = nowMs;
        return;
    case Outcome::Left:
        m_Held = false;
        // What it wrote and did not keep: the last. One that dialled (it
        // stopped at step 3 or 4, or had both dials made and was stopped by
        // what the router said when asked again before the write): its hour.
        if (verdict.step == 5)
            m_Over = true;
        else if (dialled || verdict.step == 3 || verdict.step == 4)
            m_NotBeforeMs = nowMs + kApartMs;
        return;
    }
}

void Pace::movedAway(uint64_t nowMs) noexcept
{
    m_Held = false;
    // At once, once: an address that changed may well stay so. Changed again
    // within the hour, it is not to be followed at every look.
    m_NotBeforeMs = m_MovedBefore && nowMs - m_MovedMs < kApartMs ? nowMs + kApartMs : nowMs;
    m_MovedBefore = true;
    m_MovedMs = nowMs;
}

Keeper::Keeper(IUpnpGateway& gateway, mw::routerholes::IRecord* record, IDialler& dialler,
               std::function<QString()> routerAddress)
    : m_Guard(gateway, record)
    , m_Dialler(dialler)
    , m_RouterAddress(std::move(routerAddress))
{}

void Keeper::retire()
{
    if (m_Run == 0) return;
    // Its dial, if the dialler has it, is not waited for any more: it ends
    // soon, and what it says is dropped (dialled()). One that still waited
    // here is never made.
    if (m_Out == m_Run) m_Dialler.cancel();
    m_Run = 0;
    m_Next = Dial{};
}

void Keeper::giveUp(QList<Line>& out, bool moved, uint64_t nowMs)
{
    if (m_Run == 0) return;
    retire();
    switch (m_Guard.abandon()) {
    // Nothing was written, and nothing is said
    case Guard::Abandoned::Nothing: break;
    case Guard::Abandoned::Undone:
        out.append({false, QStringLiteral("TCP 443: %1").arg(takenOffText())});
        break;
    // The router keeps what it was to take off: whoever switched something
    // off is not told it is gone
    case Guard::Abandoned::Stays:
        out.append({true, QStringLiteral("TCP 443: %1").arg(keptText(5))});
        break;
    }
    // A switch, the shared port: this host's own doing, which does not count
    // against the hour. The address browsers reach it at comes from outside,
    // and is paced as a lost entry is
    if (moved)
        m_Pace.movedAway(nowMs);
    else
        m_Pace.anew();
}

void Keeper::handOver(QList<Line>& out)
{
    // One dial at a time: the next one waits until the one the dialler has
    // is answered, wanted still or not (dialled())
    if (m_Next.kind == Dial::Kind::None || m_Out != 0) return;
    const Dial next = m_Next;
    m_Next = Dial{};
    if (m_Dialler.ask(m_Run, next)) {
        m_Out = m_Run;
        return;
    }
    // No dial can be made. The run cannot go on, and is not read as a dial
    // that said no: it is given up, what it wrote is taken off, and none is
    // begun again by this run of the server.
    m_Unable = true;
    m_Run = 0;
    if (m_Guard.abandon() == Guard::Abandoned::Stays)
        out.append({true, QStringLiteral("TCP 443: %1").arg(keptText(5))});
    out.append({true, QStringLiteral("TCP 443 is left as it is: this host could not dial to check "
                                     "it, and it is not asked for again until the server "
                                     "restarts")});
}

void Keeper::goOn(const Step& step, uint64_t nowMs, bool dialled, QList<Line>& out)
{
    if (step.over()) {
        m_Run = 0;
        // One the guard says to pace as a run that dialled is paced so
        say(step.verdict, nowMs, dialled || step.apart, out);
        return;
    }
    m_Next = step.dial;
    handOver(out);
}

void Keeper::say(const Verdict& verdict, uint64_t nowMs, bool dialled, QList<Line>& out)
{
    m_Pace.said(verdict, nowMs, dialled);
    if (verdict.outcome == Outcome::Held) {
        out.append({false, QStringLiteral("TCP 443: the router forwards it to the streams' "
                                          "shared port %1 (TCP only)")
                               .arg(m_Guard.leadsTo())});
        m_Left.clear();
    } else if (m_Pace.over()) {
        // The router wrote, or kept, something else than it was asked: not
        // asked again by this run of the server
        out.append({true, QStringLiteral("TCP 443 is left as it is: %1 (step %2), and is not "
                                         "asked for again until the server restarts")
                              .arg(verdict.said)
                              .arg(verdict.step)});
    } else if (verdict.said != m_Left) {
        // Once for each reason, not at every look
        m_Left = verdict.said;
        out.append({false, QStringLiteral("TCP 443 is left as it is: %1 (step %2)")
                               .arg(verdict.said)
                               .arg(verdict.step)});
    }
}

QList<Keeper::Line> Keeper::dialled(uint32_t run, bool result, uint64_t nowMs)
{
    QList<Line> out;
    // Not the dial the dialler has: nothing
    if (run == 0 || run != m_Out) return out;
    // Answered: the dialler has none now
    m_Out = 0;
    if (run != m_Run) {
        // Of a run that was given up: dropped here, before anything else. It
        // moves no run, and is neither paced nor said. The run under way, if
        // one is, may now have its dial made.
        handOver(out);
        return out;
    }
    goOn(m_Guard.after(result), nowMs, true, out);
    return out;
}

QList<Keeper::Line> Keeper::release()
{
    QList<Line> out;
    const bool held = m_Guard.held();
    const bool wrote = m_Guard.wrote();
    retire();
    if (!held && !wrote) {
        // Nothing of this run's is on the router, and it is asked nothing
        m_Guard.abandon();
        return out;
    }
    // Held, or just written by the run that was under way: taken off, and
    // the removal read back. One the router keeps stays in the record, and
    // that is said.
    if (!m_Guard.release())
        out.append({true, QStringLiteral("TCP 443: %1").arg(keptText(held ? 6 : 5))});
    return out;
}

QList<Keeper::Line> Keeper::look(Situation now, bool ask, uint64_t nowMs)
{
    QList<Line> out;
    if (m_Run != 0) {
        // A run is under way, on what it was begun on. Still so: one run at
        // a time. Nothing is begun, and the router is asked nothing either:
        // the run began with all a look asks, and what it noted since is its
        // own. No longer so (a switch, the shared port, the address): it is
        // given up, and the look goes on with what stands now.
        if (ask && now.wanted && now.blockPort == m_Stands.blockPort &&
            now.publicAddress == m_Stands.publicAddress)
            return out;
        giveUp(out, now.publicAddress != m_Stands.publicAddress, nowMs);
    }
    if (m_Guard.held()) {
        // Still standing on what it stood on: written again, read back.
        // Otherwise (a switch, the shared port moved or lost its hole): taken
        // off, and that is this host's own doing.
        const bool stands = ask && now.wanted && now.blockPort == m_Guard.leadsTo();
        Verdict verdict;
        bool stays = false;
        if (stands) {
            if (m_RouterAddress) now.routerAddress = m_RouterAddress();
            verdict = m_Guard.renew(now);
            m_Pace.said(verdict, nowMs);
        } else {
            // Taken off here, whatever `now` holds: nothing is written again
            // for an entry that no longer stands. One the router keeps is
            // said to be still there: whoever switched something off is not
            // told it is gone.
            stays = !m_Guard.release();
            verdict = {Outcome::Lost, 6, stays ? keptText(6) : takenOffText()};
            m_Pace.anew();
        }
        if (verdict.outcome == Outcome::Held) return out;
        // The router's doing is a warning; this host's own is not
        out.append({stands || stays, QStringLiteral("TCP 443: %1").arg(verdict.said)});
        m_Left.clear();
        // No longer held: what stands now may be asked for in this same
        // look, when a run is due
    }
    // Switched off: the router is not asked anything more
    if (!ask) return out;

    // An entry this installation wrote under an address this machine no
    // longer has is taken off whatever the switches say: that address may be
    // another machine's by now. Said once when the router will not, and never
    // with the address.
    if (m_Guard.formerAddress() == Guard::Former::Refused && !m_FormerSaid) {
        m_FormerSaid = true;
        out.append({true, QStringLiteral("TCP 443: the router still has an entry this "
                                         "installation wrote under an address this machine no "
                                         "longer has, and will not remove it: it has to be "
                                         "removed on the router itself")});
    }

    if (!now.wanted || now.blockPort == 0 || m_Unable || !m_Pace.due(nowMs)) return out;
    if (m_RouterAddress) now.routerAddress = m_RouterAddress();
    // A run: what asks the router now, here; what dials, by the dialler, with
    // this thread free meanwhile. It ends in this look (steps 0 to 2), or in
    // a dialled() to come.
    const Step step = m_Guard.begin(now);
    if (!step.over()) {
        if (++m_Runs == 0) ++m_Runs;
        m_Run = m_Runs;
        m_Stands = now;
    }
    goOn(step, nowMs, false, out);
    return out;
}

} // namespace mw::tcp443
