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

#include <QString>

/// `--native-bench <spec>`: run the native capture & encode engine on one
/// display for a while, with no network and no browser, and write what every
/// frame cost.
///
/// The measuring instrument of the encoder benchmarks. Everything the plan
/// wants to compare — preset, tuning, VBV, chroma, codec — is judged on the
/// same three numbers per frame: how long the host took (present → encoded,
/// stage by stage), how big the frame was, and the average QP the encoder
/// reports, which at a fixed bitrate is the objective proxy for sharpness.
///
/// `spec` is a comma-separated key=value list; see the usage text in the .cpp.
/// Without `display=` it lists the displays and exits. Writes one CSV row per
/// frame, prints a summary on stdout, and returns the process exit code.
int runNativeBenchCommand(const QString& spec);
