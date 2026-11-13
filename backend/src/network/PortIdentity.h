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

#include "SocketTables.h"

#include <QString>

#include <cstdint>
#include <functional>

/**
 * Whose ports this process walks: the installed host's, or a DEV identity's
 * own (RouterPortPools.h).
 *
 * A DEV identity — the DEV build, or --dev — keeps to ports of its own, so
 * that it never takes one from the host installed beside it. That is also why
 * it can never be tried on the ports a restrictive network lets out. The bench
 * key "debug_dev_standard_ports" (AppSettings) lifts it for one run, on a
 * machine where nothing holds those ports: the instance stays a DEV identity
 * in everything else — its name, its state, its HTTP ports, the staging
 * server — and walks the installed host's ports.
 *
 * Asked for and refused, the instance keeps its own ports: the key never makes
 * a start fail, and never takes a port from a program that has it.
 */
namespace mw::portidentity {

/// What the bench key comes to.
enum class Bench
{
    /// Not set.
    Off,
    /// Set on an installed host, which walks those ports as it is.
    NotDev,
    /// Refused: something on this machine holds one of the conferencing ports.
    Held,
    /// Refused: the system would not say what holds them.
    Unknown,
    /// This DEV identity walks the installed host's ports, for this run.
    Granted,
};

struct Verdict
{
    Bench bench = Bench::Off;
    /// The port found held, and in which protocol (Bench::Held).
    uint16_t port = 0;
    bool tcp = false;
};

/// mw::sockettables::listens, or what stands for it in a test.
using Listens = std::function<mw::sockettables::Listens(uint16_t port, bool tcp)>;

/// What the key comes to for an identity. `listens` is asked about each of
/// the eight conferencing ports, in UDP and in TCP, and only when the key is
/// set on a DEV identity. One port held refuses, whatever the others say; one
/// answer that says nothing refuses too.
Verdict benchVerdict(bool devIdentity, bool key, const Listens& listens);

/// Whether an identity walks a DEV identity's ports, given what its key came to.
constexpr bool devPortsFor(bool devIdentity, Bench bench)
{
    return devIdentity && bench != Bench::Granted;
}

/// What the log says of a verdict. Empty when the key is not set.
QString said(const Verdict& verdict);

// ── This process ─────────────────────────────────────────────────────────────

/// Settles it for the run, from the system's own tables. Once, as the server
/// starts and before it asks for any port: what holds a port later — this
/// process included — changes nothing.
Verdict settle(bool key);

/// Whether this process walks a DEV identity's ports. mw::edition::isDev()
/// until settle() has said otherwise.
bool devPorts();

} // namespace mw::portidentity
