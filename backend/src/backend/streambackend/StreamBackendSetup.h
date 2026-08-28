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

#include <QByteArray>
#include <QString>
#include <functional>

class NvComputer;
class NvHTTP;
class QNetworkAccessManager;

/**
 * @brief Populates StreamBackendRegistry with the providers this build knows.
 *
 * A free function rather than a ComputerManager method because **two processes
 * need it**. The server has a ComputerManager and a whole host list; the stream
 * worker is a separate process that rebuilds a single NvComputer from JSON and
 * has neither. Both must be able to reach a backend, so the two dependencies
 * that actually differ — how to find a host, and what a completed pairing means
 * — are parameters instead of captured state.
 */
namespace StreamBackendSetup {

/// Resolve a host by uuid. May return nullptr: a host can be deleted while a
/// request against it is in flight.
using HostLookup = std::function<NvComputer*(const QString& uuid)>;

/// Record a pairing a backend completed on its own (Wolf). The server writes
/// the certificate to the host and saves; the worker has nothing to persist and
/// passes an empty function.
using PairingCommit = std::function<void(const QString& uuid, const QByteArray& serverCertPem)>;

/// Registers every known type. Safe to call once per process; the registry
/// keeps the last registration for a type.
void registerAll(NvHTTP* http, QNetworkAccessManager* nam, HostLookup lookup, PairingCommit commit);

} // namespace StreamBackendSetup
