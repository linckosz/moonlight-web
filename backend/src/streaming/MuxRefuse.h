#pragma once

// A stream the server counted on its shared ports that ends up on a socket of
// its own (MUXREFUSE). The server decides before the launch that a stream
// shares, and a stream that shares asks the router for no hole of its own. Two
// things can still leave it on its own port: the server cannot make the share
// (StreamWorkerHost::withSharedSockets), or the worker refuses what it is given
// (IceMuxClient::configure). Either way that worker says so once, the server
// claims the hole the slot would have had without the mux, and tells it.
//
// The rules below are the ones that keep this from opening more than a stream
// without the mux always has: the slot's own hole, once per worker process,
// never while the server quits, and kept or taken off afterwards exactly as a
// hole claimed before the launch is. A worker handles untrusted network input:
// what it sends is read strictly.

#include <cmath>

#include <QHostAddress>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace mw::muxrefuse {

/// The worker's word, and nothing else: exactly {"event":"iceMux","op":"refused"}.
/// Any other key, or any other value, is not that word.
inline bool isRefusedEvent(const QJsonObject& event)
{
    return event.size() == 2 && event.value(QStringLiteral("event")) == QStringLiteral("iceMux") &&
           event.value(QStringLiteral("op")) == QStringLiteral("refused");
}

/// The server's answer to that worker, as the start config carries a hole:
/// {"cmd":"iceMuxHole","upnpPublicIp":<address or "">,"upnpExternalPort":<0..65535>}.
/// External port 0 = no hole (the address is then empty). Anything else is
/// refused and changes nothing.
inline bool parseHoleCommand(const QJsonObject& cmd, QString* publicIp, quint16* externalPort)
{
    if (cmd.size() != 3 || cmd.value(QStringLiteral("cmd")) != QStringLiteral("iceMuxHole"))
        return false;
    const QJsonValue ip = cmd.value(QStringLiteral("upnpPublicIp"));
    const QJsonValue port = cmd.value(QStringLiteral("upnpExternalPort"));
    if (!ip.isString() || !port.isDouble()) return false;
    const double n = port.toDouble();
    if (!std::isfinite(n) || n < 0 || n > 65535 || n != std::floor(n)) return false;
    const QString address = ip.toString();
    if (n == 0) {
        if (!address.isEmpty()) return false;
    } else {
        QHostAddress parsed;
        if (address.size() > 45 || !parsed.setAddress(address)) return false;
    }
    *publicIp = address;
    *externalPort = static_cast<quint16>(n);
    return true;
}

/// Whether the server asks the router at all when a worker needs its own port.
/// Quitting: nothing (a hole claimed while releaseBeforeExit runs would stay on
/// the router). A launch on the slot that is not this one any more: nothing.
inline bool claimWhenAsked(bool quitting, bool launchCurrent)
{
    return !quitting && launchCurrent;
}

enum class Answer
{
    Use,     // tell the worker
    Release, // the server quits: take it off at once
    Guest,   // a guest's launch that is over: the guests' own release (a grace, then off)
    Nothing, // no hole came, or an owner's slot keeps it as its held hole
};

/// What to do with the router's answer. `hasHole` = the answer carries a hole.
/// An owner's slot keeps its hole past its stream by design, so that the next
/// launch on the slot has it at once (RouterPortAllocator), and it is taken off
/// at exit: a hole that comes back for an owner's launch that is over stays as
/// that slot's held hole. A guest's goes the way a guest's hole always goes.
inline Answer onAnswer(bool quitting, bool launchCurrent, bool workerLive, bool hasHole,
                       bool guestSlot)
{
    if (!hasHole) return Answer::Nothing;
    if (quitting) return Answer::Release;
    if (launchCurrent && workerLive) return Answer::Use;
    return guestSlot ? Answer::Guest : Answer::Nothing;
}

} // namespace mw::muxrefuse
