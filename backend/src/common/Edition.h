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
#include <QtGlobal>

/**
 * Which MoonlightWeb this process is, and every name that follows from it.
 *
 * Two independent switches, and three identities between them:
 *
 *   PROD build           MoonlightWeb      — a v* tag, what users install
 *   DEV build            MoonlightWebDev   — every other CI build (-DMW_EDITION=dev)
 *   --dev on either      <name>-dev        — a scratch instance beside the installed one
 *
 * The DEV build is an installable pre-release that has to live on the same
 * machine as a production install without touching it: its own launcher name
 * (Windows, macOS and Linux key firewall rules, TCC grants, services and
 * packages on it), its own state, its own ports, the staging introduction
 * server, blue icons, and a version ending in -dev. --dev keeps doing what it
 * always did — isolated state, alternate ports, no single-instance lock — and
 * also wears the DEV look, so a scratch instance cannot be mistaken for the
 * installed one in the tray either.
 *
 * The state name of a plain --dev run stays "MoonlightWeb-dev": that is where
 * every existing dev instance keeps its pairings, and renaming it would unpair
 * every bench machine.
 */
namespace mw::edition {

// Ports a DEV identity listens on by default, clear of the production 80/443
// and of the GameStream ranges (47984-48010 stock, 48100+ for MultiSeat seats).
// The signaling base also seeds the control channel (+2) and the per-slot
// worker ports (+10 * slot), so it moves as a block.
constexpr quint16 kDevHttpPort = 48080;
constexpr quint16 kDevHttpsPort = 48443;
constexpr quint16 kDevSignalingPort = 48501;

// ── Pure forms (what the process-wide answers below are made of) ────────────

/// Application name, which decides where settings.json, the lock, the logs and
/// the QSettings host list live.
QString dataNameFor(bool devBuild, bool devFlag);

/// What a person reads: tray tooltip, notifications, shortcuts.
QString displayNameFor(bool devBuild, bool devFlag);

/// The version as shown and reported: `base`, plus "-dev" for a DEV identity
/// (never twice — the untagged fallback is already 0.0.0-dev).
QString versionFor(const QString& base, bool dev);

/// The launcher's name: what the installer registered this build under (the
/// executable, the service, the scheduled tasks, the LaunchAgent). A property
/// of the build alone — a --dev run of the production binary is still started
/// from MoonlightWeb.exe, and must not go looking for a DEV install's tasks.
QString productNameFor(bool devBuild);

// ── This process ─────────────────────────────────────────────────────────────

/// Read --dev from argv. Call once, before anything asks the questions below —
/// the application name has to be settled before any state is touched.
void init(int argc, char** argv);

/// Compiled as the DEV edition (-DMW_EDITION=dev).
bool isDevBuild();
/// Started with --dev.
bool devFlag();
/// Either of the two: wears the DEV name, icons, ports and staging server.
bool isDev();

QString dataName();
QString displayName();
QString productName();
/// MW_VERSION, with the DEV suffix when isDev().
QString version();

quint16 defaultHttpPort();
quint16 defaultHttpsPort();

/// Frontend-relative path of an icon: the blue variant under assets/dev/ for a
/// DEV identity, the production one under assets/ otherwise.
QString iconAsset(const QString& file);

// ── LAN-only (MW_LAN_ONLY) ───────────────────────────────────────────────────
//
// For a fork developing on its own LAN (CLAUDE-LAN-DEV-SETUP.md): the process
// reaches none of the project's servers — no rendezvous line, no STUN, no UPnP
// mapping, no update check, no census — and anything asking to turn Internet
// Access on is answered with lanOnlyRefusal() instead.

/// 1 / true / yes / on (any case) turn it on; anything else leaves it off.
bool lanOnlyFrom(const QByteArray& value);

/// MW_LAN_ONLY from the environment (.env included); when unset, the value a
/// build configured with MW_LAN_ONLY in its environment embedded.
bool lanOnly();

/// The error every refused Internet Access request answers with.
QString lanOnlyRefusal();

} // namespace mw::edition
