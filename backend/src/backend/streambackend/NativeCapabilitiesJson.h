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

#include "mw/native/Capabilities.h"

#include <QJsonObject>

/**
 * The native engine's probe result as JSON — the shape it travels in between
 * the `--native-probe` child (which runs where the desktop is) and the server
 * (which may not).
 *
 * Enums go over as their numeric values: both ends are the same executable, so
 * the numbers agree by construction, and a `schema` field guards the one case
 * where they might not — an update replaced the file on disk under a running
 * service. A reader that sees another schema rejects the whole object rather
 * than trusting half of it.
 */
namespace NativeCapabilitiesJson {

constexpr int kSchema = 1;

QJsonObject toJson(const mw::native::Capabilities& caps);

/// False when `obj` is not a probe result of this schema; `out` is then left
/// as an unavailable result with ProbeFailed and a diagnostic.
bool fromJson(const QJsonObject& obj, mw::native::Capabilities& out);

} // namespace NativeCapabilitiesJson
