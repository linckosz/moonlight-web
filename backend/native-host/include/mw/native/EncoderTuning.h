/*
 * MoonlightWeb — native capture & encoding engine.
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

#include <string>

namespace mw::native {

/// The encoder knobs the engine decides for itself — exposed so the BENCH can
/// move them, one at a time, and measure what each one costs.
///
/// ── This is not configuration ───────────────────────────────────────────────
///
/// Nothing in the product sets any of these: the engine's own choice is what
/// every field's `Default` means, and a session started from a browser never
/// carries anything else (§28 of the mission — no preset, no tuning, no VBV in
/// front of a user). The struct exists because the choice has to be MEASURED
/// before it is made, on every vendor's silicon, and a benchmark that cannot
/// vary the setting under test is not one. `--native-bench` is the only caller
/// that fills it in.
///
/// Tri-state on purpose: "leave it to the preset" is a distinct answer from
/// "off", and the difference is exactly what the bench wants to see — what a
/// vendor's ultra-low-latency preset actually enables, and whether it was right.
struct EncoderTuning
{
    enum class Choice
    {
        Default,
        Off,
        On
    };

    /// NVENC: the P1 (fastest) … P7 (best quality) preset. 0 is the engine's
    /// own (P1 since the bench of 04/09/2026 — see NvencEncoder.cpp).
    int nvencPreset = 0;

    /// NVENC: which latency tuning the preset is fetched for.
    enum class Latency
    {
        Default,
        UltraLow,
        Low
    };
    Latency nvencTuning = Latency::Default;

    /// NVENC: a first pass at reduced or full resolution to steer the rate
    /// control of the second. Costs encode time; buys a steadier CBR.
    enum class MultiPass
    {
        Default,
        Off,
        QuarterRes,
        FullRes
    };
    MultiPass nvencMultiPass = MultiPass::Default;

    /// Spatial adaptive quantization: NVENC `enableAQ`, AMF VBAQ, AMF AV1 CAQ.
    Choice spatialAq = Choice::Default;
    /// NVENC only: temporal AQ.
    Choice temporalAq = Choice::Default;
    /// AMF only: the pre-analysis module ahead of the rate control.
    Choice preAnalysis = Choice::Default;

    /// AMF: the quality preset — speed, balanced, quality. Default is whatever
    /// the ultra-low-latency usage picks.
    enum class AmfQuality
    {
        Default,
        Speed,
        Balanced,
        Quality
    };
    AmfQuality amfQuality = AmfQuality::Default;

    /// oneVPL: TargetUsage 1 (quality) … 7 (speed). 0 is the engine's own (7).
    int vplTargetUsage = 0;

    /// NVENC: how many frames the decoded picture buffer holds. 0 is the
    /// engine's own (4, so a lost frame can be healed by a delta — see
    /// NvencEncoder); 1 is the bench's "before" for the cost of that.
    int dpbFrames = 0;

    /// The VBV, in frames at the stream's own rate — exactly, with no floor.
    /// 0 is the engine's rule: one frame, never less than a sixtieth of a
    /// second's worth (RateControl.h says why). 1 and 2 are the two bounds the
    /// bench compares that rule against.
    int vbvFrames = 0;

    bool isDefault() const
    {
        return nvencPreset == 0 && nvencTuning == Latency::Default &&
               nvencMultiPass == MultiPass::Default && spatialAq == Choice::Default &&
               temporalAq == Choice::Default && preAnalysis == Choice::Default &&
               amfQuality == AmfQuality::Default && vplTargetUsage == 0 && vbvFrames == 0 &&
               dpbFrames == 0;
    }

    /// One line naming every field that is NOT at its default, for the log and
    /// the bench summary. Empty when nothing is.
    std::string describe() const
    {
        std::string s;
        auto add = [&s](const std::string& item) {
            if (!s.empty()) s += ' ';
            s += item;
        };
        auto choice = [](Choice c) { return c == Choice::On ? "on" : "off"; };
        if (nvencPreset > 0) add("preset=P" + std::to_string(nvencPreset));
        if (nvencTuning == Latency::UltraLow) add("tuning=ULL");
        if (nvencTuning == Latency::Low) add("tuning=LL");
        if (nvencMultiPass == MultiPass::Off) add("multipass=off");
        if (nvencMultiPass == MultiPass::QuarterRes) add("multipass=quarter");
        if (nvencMultiPass == MultiPass::FullRes) add("multipass=full");
        if (spatialAq != Choice::Default) add(std::string("aq=") + choice(spatialAq));
        if (temporalAq != Choice::Default) add(std::string("taq=") + choice(temporalAq));
        if (preAnalysis != Choice::Default) add(std::string("preanalysis=") + choice(preAnalysis));
        if (amfQuality == AmfQuality::Speed) add("quality=speed");
        if (amfQuality == AmfQuality::Balanced) add("quality=balanced");
        if (amfQuality == AmfQuality::Quality) add("quality=quality");
        if (vplTargetUsage > 0) add("tu=" + std::to_string(vplTargetUsage));
        if (vbvFrames > 0) add("vbv=" + std::to_string(vbvFrames) + "f");
        if (dpbFrames > 0) add("dpb=" + std::to_string(dpbFrames));
        return s;
    }
};

} // namespace mw::native
