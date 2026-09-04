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

#include "mw/native/EncoderTuning.h"

#include <QString>

/// Parse the encoder-knob part of a bench spec — `preset=1,multipass=off,
/// aq=1,…,gpu=<id>` — into @p tuning and @p gpu (-1 when absent). Shared with
/// the `MW_NATIVE_TUNING` environment variable, which lets a real session be
/// started with the same overrides for an A/B the bench cannot do: two
/// settings looked at, by a person, on the same screen. Unknown keys are an
/// error; the bench's own keys (display, seconds, codec…) are not accepted
/// here. Returns false and fills @p error on a bad key or value.
bool parseEncoderTuningSpec(const QString& spec, mw::native::EncoderTuning& tuning, int& gpu,
                            QString& error);

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
