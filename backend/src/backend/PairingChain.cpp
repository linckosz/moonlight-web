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

#include "PairingChain.h"

#include "NvPairingManager.h"

#include <QRandomGenerator>
#include <QTimer>
#include <memory>

namespace PairingChain {

namespace {
// Long enough for stage 1 to reach a host on the far side of a LAN, short
// enough that nobody waits on it — the host is going to sit on that request
// for up to a minute anyway.
constexpr int kPinAnnounceDelayMs = 3000;
} // namespace

QString generatePin()
{
    int pin = QRandomGenerator::global()->bounded(10000);
    return QString::asprintf("%04d", pin);
}

void run(NvPairingManager* pm, const QString& pin, PinAnnouncer announcer, ResultCallback cb)
{
    if (!pm) {
        cb(Result{Outcome::Failed, {}});
        return;
    }

    // initiatePairing() answers synchronously when stage 1 already succeeded on
    // an earlier attempt (NvPairingManager::m_Stage1Done). Track that, because
    // the announcer must not fire on that path: no fresh request went out, so
    // there is no new pairing request on the host waiting for a PIN.
    //
    // Only read before run() returns, so a later async completion cannot be
    // mistaken for a synchronous one.
    auto settledSynchronously = std::make_shared<bool>(false);

    auto report = [cb](const Result& result) { cb(result); };

    pm->initiatePairing(
        [pm, pin, report, settledSynchronously](NvPairingManager::InitResult initResult) {
            *settledSynchronously = true;

            if (initResult == NvPairingManager::INIT_ALREADY_IN_PROGRESS) {
                report(Result{Outcome::HostBusy, {}});
                return;
            }

            if (initResult != NvPairingManager::INIT_OK) {
                // Stage 1 timed out or the host was unreachable. Non-terminal: the
                // caller keeps the session and may run the chain again.
                report(Result{Outcome::Retry, {}});
                return;
            }

            // Stages 2-5 — challenge/response. `pm` is borrowed and outlives this
            // by contract; the caller pins it for the whole chain.
            pm->completePairing(
                pin, [report](NvPairingManager::PairState state, const QByteArray& serverCertPem) {
                    switch (state) {
                    case NvPairingManager::PAIRED:
                        report(Result{Outcome::Paired, serverCertPem});
                        break;

                    case NvPairingManager::PIN_WRONG:
                        // Not accepted yet — non-terminal, same as stage-1 failure.
                        report(Result{Outcome::Retry, {}});
                        break;

                    case NvPairingManager::ALREADY_IN_PROGRESS:
                    case NvPairingManager::FAILED:
                    default: report(Result{Outcome::Failed, {}}); break;
                    }
                });
        });

    // Reaching here with nothing reported means stage 1 went to the network and
    // will be parked on the host waiting for a PIN. That is the only moment an
    // out-of-band PIN push makes sense — and for Wolf it is the only thing that
    // will ever unblock the response.
    //
    // "Went to the network" is not "arrived": the request is dispatched
    // asynchronously, so announcing here can beat it to the host. A host that
    // has not parked it yet has nothing to give the PIN to. Let the dispatch
    // finish first — harmless for Wolf, whose request blocks server-side, and
    // for a loopback Sunshine, which is simply ready sooner.
    if (announcer && !*settledSynchronously) {
        QTimer::singleShot(kPinAnnounceDelayMs, [announcer = std::move(announcer), pin]() {
            announcer(pin);
        });
    }
}

} // namespace PairingChain
