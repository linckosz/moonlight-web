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

    /// AMF H.264/HEVC: the encoder's internal low-latency mode
    /// (`LowLatencyInternal`, which also puts H.264 in POC mode 2).
    ///
    /// Its own header says `default = false` — flatly, not "depends on USAGE"
    /// like every other knob here — so the ultra-low-latency usage is not
    /// documented to switch it on. Worth a measurement rather than a guess: it
    /// is the one AMD knob this engine has never touched. AV1 has no such
    /// property; it has an explicit latency mode, already set to its lowest.
    Choice amfLowLatency = Choice::Default;

    /// oneVPL: TargetUsage 1 (quality) … 7 (speed). 0 is the engine's own (7).
    int vplTargetUsage = 0;

    // ── The rest of what Intel exposes ──────────────────────────────────────
    //
    // Six knobs, none of which this engine sets on its own except the first,
    // and all of which the Intel matrix measures rather than assumes. They live
    // here for the same reason every other field does: a benchmark that cannot
    // vary the setting under test is not one.

    /// The fixed-function encode engine (VDENC) rather than the shader-based
    /// one. The engine's own answer is ON — measured, it halves the encode time
    /// — with an automatic fall-back when a generation has none.
    Choice vplLowPower = Choice::Default;
    /// Macroblock-level rate control: spends bits where the picture needs them
    /// rather than evenly. Intel's answer to spatial AQ.
    Choice vplMbBrc = Choice::Default;
    /// The alternative bitrate controller. Intel documents it as better on
    /// low-delay content, which is exactly this pipeline's content.
    Choice vplExtBrc = Choice::Default;
    /// The low-delay mode of the bitrate controller — one frame in, one frame
    /// out, no lookahead budgeting.
    Choice vplLowDelayBrc = Choice::Default;
    /// `ScenarioInfo = MFX_SCENARIO_REMOTE_GAMING`, a hint Intel added for this
    /// exact use. What the driver does with it is not documented, which is why
    /// it is measured.
    Choice vplGamingScenario = Choice::Default;
    /// Sliding-window rate cap, in frames: no window of this many frames may
    /// average more than the target. A burst limiter, priced in quality.
    int vplWinBrcFrames = 0;

    /// How many reference pictures the encoder keeps for healing a lost frame
    /// by a delta. NVENC: the decoded picture buffer's depth (engine's own: 4
    /// — see NvencEncoder). AMF: the number of long-term reference slots
    /// (engine's own: 4 — see AmfEncoder / ReferenceSlots). 0 is the engine's
    /// own; 1 is the bench's "before" — a single reference, no invalidation
    /// possible on either vendor — for the cost of the feature.
    int dpbFrames = 0;

    /// The VBV, in frames at the stream's own rate — exactly, with no floor.
    /// 0 is the engine's rule: one frame, never less than a sixtieth of a
    /// second's worth (RateControl.h says why). 1 and 2 are the two bounds the
    /// bench compares that rule against.
    int vbvFrames = 0;

    /// Bench only: pretend no GPU in this machine can encode, so the session
    /// lands on the fallback tier — and, optionally, on ONE named member of it.
    /// The only way to exercise Media Foundation or the CPU encoder on a bench
    /// that has NVENC, and to price them against it on the same content.
    enum class Fallback
    {
        None,                    ///< the engine's choice: GPUs first, the tier only without them
        Tier,                    ///< whatever the tier would pick on an encoder-less machine
        MediaFoundation,         ///< the Media Foundation transform, hardware or software
        MediaFoundationSoftware, ///< Microsoft's software transform even where hardware exists
        MediaFoundationCpuInput, ///< the hardware transform, fed through system memory
        Cpu                      ///< OpenH264
    };
    Fallback fallback = Fallback::None;

    bool isDefault() const
    {
        return nvencPreset == 0 && nvencTuning == Latency::Default &&
               nvencMultiPass == MultiPass::Default && spatialAq == Choice::Default &&
               temporalAq == Choice::Default && preAnalysis == Choice::Default &&
               amfQuality == AmfQuality::Default && amfLowLatency == Choice::Default &&
               vplTargetUsage == 0 && vplLowPower == Choice::Default &&
               vplMbBrc == Choice::Default && vplExtBrc == Choice::Default &&
               vplLowDelayBrc == Choice::Default && vplGamingScenario == Choice::Default &&
               vplWinBrcFrames == 0 && vbvFrames == 0 && dpbFrames == 0 &&
               fallback == Fallback::None;
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
        if (amfLowLatency != Choice::Default)
            add(std::string("lowlatency=") + choice(amfLowLatency));
        if (vplTargetUsage > 0) add("tu=" + std::to_string(vplTargetUsage));
        if (vplLowPower != Choice::Default) add(std::string("lowpower=") + choice(vplLowPower));
        if (vplMbBrc != Choice::Default) add(std::string("mbbrc=") + choice(vplMbBrc));
        if (vplExtBrc != Choice::Default) add(std::string("extbrc=") + choice(vplExtBrc));
        if (vplLowDelayBrc != Choice::Default)
            add(std::string("lowdelaybrc=") + choice(vplLowDelayBrc));
        if (vplGamingScenario != Choice::Default)
            add(std::string("gaming=") + choice(vplGamingScenario));
        if (vplWinBrcFrames > 0) add("winbrc=" + std::to_string(vplWinBrcFrames) + "f");
        if (vbvFrames > 0) add("vbv=" + std::to_string(vbvFrames) + "f");
        if (dpbFrames > 0) add("dpb=" + std::to_string(dpbFrames));
        if (fallback == Fallback::Tier) add("fallback=1");
        if (fallback == Fallback::MediaFoundation) add("fallback=mf");
        if (fallback == Fallback::MediaFoundationSoftware) add("fallback=mfsw");
        if (fallback == Fallback::MediaFoundationCpuInput) add("fallback=mfcpu");
        if (fallback == Fallback::Cpu) add("fallback=cpu");
        return s;
    }
};

} // namespace mw::native
