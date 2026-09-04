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

#include <cstddef>
#include <cstdint>

namespace mw::native::encode {

/// How many bits one frame may occupy — the VBV size, shared by every encoder.
///
/// ── What this actually bounds ───────────────────────────────────────────────
///
/// A VBV of B bits at rate R means a frame is at most B bits, so it occupies
/// B/R seconds of the link. Setting B = R/fps therefore says "a frame must
/// transmit within one frame interval", which is the right bound while frames
/// really do arrive at fps.
///
/// ── Why there is a floor, and why it is 60 ──────────────────────────────────
///
/// Two things break that reasoning at high refresh rates.
///
/// First, a desktop does not produce frames at the panel's refresh rate. Desktop
/// Duplication delivers on damage, so a page of text produces almost nothing —
/// yet a 165 Hz panel made every frame budget 1/165th of the bitrate, keyframe
/// included. 55 Mbps became 41 KB for a full 1080p intra frame, which is a
/// heavily quantized, visibly soft picture. On a moving screen the next frame
/// refines it and nobody notices. On a still one nothing ever follows, so the
/// soft first frame IS the picture, for as long as the user looks at it.
///
/// Second, the tightening buys nothing. 1/60 s of link occupancy is already
/// below the frame interval of every client display in use, so squeezing a frame
/// from 16.7 ms of link time down to 6 ms removes quality without removing any
/// delay a viewer could perceive.
///
/// So: one frame's worth at the stream's rate, never less than one sixtieth of a
/// second's worth. Above 60 the budget stops shrinking; at or below 60 the
/// original per-frame bound is unchanged, because there the content really can
/// arrive that fast and the bound is doing real work.
///
/// The proper fix is a VBV that follows the rate frames are ACTUALLY produced at
/// rather than the rate they might be — that is the client-driven rate control
/// of §9.3, and it needs the client feedback loop first. This floor is the part
/// that is correct on its own.
constexpr int kVbvFrameRateFloor = 60;

inline uint32_t vbvBitsPerFrame(uint32_t bitsPerSecond, int fps)
{
    const int rate = (fps > kVbvFrameRateFloor) ? kVbvFrameRateFloor : (fps > 0 ? fps : 60);
    return bitsPerSecond / static_cast<uint32_t>(rate);
}

/// The VBV the encoders actually configure: the rule above, unless the bench
/// asked for exactly @p frames frames' worth at the stream's rate, floor and
/// all set aside. That is how the rule gets measured against its alternatives
/// rather than assumed — see EncoderTuning::vbvFrames.
inline uint32_t vbvBits(uint32_t bitsPerSecond, int fps, int frames)
{
    if (frames <= 0) return vbvBitsPerFrame(bitsPerSecond, fps);
    const int rate = fps > 0 ? fps : 60;
    return static_cast<uint32_t>(static_cast<uint64_t>(bitsPerSecond) *
                                 static_cast<uint32_t>(frames) / static_cast<uint32_t>(rate));
}

/// How long one intra-refresh sweep takes, in frames — shared by every encoder.
///
/// ── Why a duration, expressed in frames of the EFFECTIVE rate ───────────────
///
/// The wave replaces the periodic keyframe: every frame carries a band of intra
/// blocks, and once the band has crossed the whole picture a receiver that
/// joined late or decoded through a loss is whole again. What matters to that
/// receiver is how long the repair takes in seconds — the browser's ride-out
/// watchdog gives the wave about that long before demanding a keyframe — and
/// what matters to the link is how many extra bits each frame carries, which is
/// the picture divided by the number of frames in the sweep.
///
/// Two seconds is the balance: short enough that a damaged picture heals before
/// anyone reads it as a fault, long enough that the per-frame surcharge stays
/// small. The three vendor paths used to hard-code 120 frames, which is two
/// seconds only at 60 fps: a 30 fps stream took four seconds to heal (longer
/// than the client's watchdog, so it asked for the keyframe anyway and paid for
/// both), and a 144 fps stream swept in 0.8 s, spending two and a half times
/// the intra bits per second that it needed to.
///
/// The rate handed in is the one frames are actually encoded at — the setting,
/// or the display's own refresh when the setting is 0 — never the panel's rate
/// when the stream is gated below it. An unknown rate reads as 60, like the VBV.
/// The bounds only guard against nonsense: below 30 frames the band would be a
/// visible stripe, above 600 (five seconds at 120 fps) the sweep is not a
/// repair any more.
constexpr int kIntraRefreshSeconds = 2;
constexpr int kIntraRefreshMinFrames = 30;
constexpr int kIntraRefreshMaxFrames = 600;

inline int intraRefreshPeriodFrames(int fps)
{
    const int rate = fps > 0 ? fps : 60;
    const int period = rate * kIntraRefreshSeconds;
    if (period < kIntraRefreshMinFrames) return kIntraRefreshMinFrames;
    if (period > kIntraRefreshMaxFrames) return kIntraRefreshMaxFrames;
    return period;
}

/// How many frames of the period the band is actually moving, for the encoders
/// that separate the two (NVENC). Half the period: shorter concentrates the
/// extra bits into fewer frames; spreading it keeps the bitrate flat, which is
/// the whole point of using intra-refresh instead of keyframes.
inline int intraRefreshCountFrames(int fps)
{
    return intraRefreshPeriodFrames(fps) / 2;
}

/// The rate frames are REALLY produced at, and what the encoder should be told
/// about it (plan v2, E4).
///
/// ── The problem it fixes ────────────────────────────────────────────────────
///
/// Constant bitrate is a budget per frame: bitrate / frame rate. The frame rate
/// the encoder is configured with is the stream's setting — or the display's
/// refresh when the setting is 0 — and a game rarely runs at either. A 60 fps
/// game on a 165 Hz display under a "165" stream produces 60 frames a second,
/// each budgeted at 1/165th of the bitrate: the wire carries 60/165 of what the
/// viewer allowed, and the picture is quantized as if the link were 2.75 times
/// smaller than it is. Measured on the bench (Call of Duty footage at 60 fps,
/// 40 Mbit/s): 29 KB per frame and QP 25 with the setting at 165, 65 KB and
/// QP 12 with the setting at 60 — same content, same link, same encoder.
///
/// ── What it does ────────────────────────────────────────────────────────────
///
/// Counts the frames actually encoded from a capture over one-second windows,
/// and holds the rate the encoder's budget is dimensioned for. The budget is
/// moved by scaling the bitrate handed to the encoder — bitrate × configured /
/// effective — rather than by reconfiguring the frame rate, because every
/// vendor path already has a live setBitrate() and none of them promises a
/// live frame-rate change. The wire still carries the configured bitrate:
/// (bitrate × configured / effective) × effective frames = bitrate. The VBV
/// scales with it, which is the point — a bigger frame is allowed because
/// fewer of them are coming.
///
/// ── Why it moves the way it does ────────────────────────────────────────────
///
/// Raising the rate (frames got MORE frequent) must be quick: each frame is now
/// too big for the interval, and every second spent at the old budget is a
/// second of frames at up to 2.75× the link — on the Internet, that is the
/// pointer trailing the hand. So frames are also counted over a quarter-second
/// sub-window, and a sub-window already faster than the current rate raises it
/// on the spot; the overshoot is bounded to a quarter of a second of frames.
/// Lowering (frames got less frequent, the budget may grow) waits for two
/// consecutive one-second windows to agree, so a hitch never inflates the next
/// second's frames. Fifteen percent of hysteresis either way keeps a game
/// hovering around a rate from reconfiguring the encoder every second. Never
/// below 30: a still screen produces almost no captures, and its frames — the
/// refinement burst — have a budget of their own (kStillBoost). Never above
/// the configured rate: that is the viewer's own statement of what the link
/// carries.
struct EffectiveCadence
{
    static constexpr int64_t kWindowUs = 1000 * 1000;
    static constexpr int64_t kRaiseWindowUs = 250 * 1000;
    static constexpr int kMinFps = 30;
    static constexpr int kHysteresisPercent = 15;
    static constexpr int kLowerAfterWindows = 2;

    int configuredFps = 60;
    /// What the encoder's budget is dimensioned for right now.
    int currentFps = 60;
    int64_t windowStartUs = 0;
    int framesInWindow = 0;
    int64_t raiseWindowStartUs = 0;
    int framesInRaiseWindow = 0;
    int lowerStreak = 0;
    /// How often the rate moved, for the log.
    int changes = 0;

    void start(int fps, int64_t nowUs)
    {
        configuredFps = fps > 0 ? fps : 60;
        currentFps = configuredFps;
        windowStartUs = nowUs;
        raiseWindowStartUs = nowUs;
        framesInWindow = 0;
        framesInRaiseWindow = 0;
        lowerStreak = 0;
    }

    /// One frame encoded from a real capture at @p nowUs. Returns true when
    /// the rate the encoder should be dimensioned for just changed.
    bool noteFrame(int64_t nowUs)
    {
        framesInWindow++;
        framesInRaiseWindow++;
        bool changed = false;

        // The quick look, for a rate that went UP.
        if (nowUs - raiseWindowStartUs >= kRaiseWindowUs) {
            const int quick = ratePerSecond(framesInRaiseWindow, nowUs - raiseWindowStartUs);
            raiseWindowStartUs = nowUs;
            framesInRaiseWindow = 0;
            changed = raiseTo(quick);
        }

        if (nowUs - windowStartUs < kWindowUs) return changed;
        const int measured = ratePerSecond(framesInWindow, nowUs - windowStartUs);
        windowStartUs = nowUs;
        framesInWindow = 0;
        return evaluate(measured) || changed;
    }

    static int ratePerSecond(int frames, int64_t spanUs)
    {
        if (spanUs <= 0) return 0;
        return static_cast<int>((static_cast<int64_t>(frames) * 1000000 + spanUs / 2) / spanUs);
    }

    /// The upward half of evaluate(): act when @p measured is clearly above
    /// the current rate. Clamped to the setting.
    bool raiseTo(int measured)
    {
        const int target = measured > configuredFps ? configuredFps : measured;
        if (target <= currentFps + currentFps * kHysteresisPercent / 100) return false;
        currentFps = target;
        lowerStreak = 0;
        changes++;
        return true;
    }

    /// The window closed with @p measured frames per second. Split out so it
    /// can be tested without a clock.
    bool evaluate(int measured)
    {
        int target = measured;
        if (target > configuredFps) target = configuredFps;
        if (target < kMinFps) target = kMinFps;

        if (raiseTo(target)) return true;
        if (target < currentFps - currentFps * kHysteresisPercent / 100) {
            if (++lowerStreak >= kLowerAfterWindows) {
                currentFps = target;
                lowerStreak = 0;
                changes++;
                return true;
            }
            return false;
        }
        lowerStreak = 0;
        return false;
    }

    /// The bitrate to hand the encoder so that a frame at the current rate
    /// gets the budget the configured bitrate implies at the configured rate.
    int scaledKbps(int kbps) const
    {
        if (currentFps <= 0 || currentFps >= configuredFps) return kbps;
        const int64_t scaled = static_cast<int64_t>(kbps) * configuredFps / currentFps;
        return static_cast<int>(scaled);
    }

    bool scaling() const { return currentFps < configuredFps; }
};

/// The budget a frame gets while the screen is not moving, as a multiple of the
/// stream's own.
///
/// ── Why the floor above was not enough ──────────────────────────────────────
///
/// The floor lifted the first 1080p keyframe from 41 KB to 112 KB. The cap it
/// was measured against is 55 Mbps / 60 / 8 = 114 KB — so the keyframe came out
/// pinned to the new ceiling exactly as it had been pinned to the old one. The
/// encoder is not choosing 112 KB, it is being cut off there, and a still 1080p
/// desktop needs three to five times that to render text without softening it.
///
/// ── Why raising it is free, here and only here ──────────────────────────────
///
/// A VBV bounds how long one frame occupies the link. That bound protects the
/// NEXT frame — and while the screen is still, there is no next frame. The link
/// is carrying nothing, so a frame that takes several frame intervals to
/// transmit delays nothing at all. The moment something moves, the ordinary
/// budget is restored before that frame is encoded, so motion never pays for it.
///
/// ── Why it is expressed as a bitrate ────────────────────────────────────────
///
/// Constant bitrate aims each frame at rate/fps bits and the VBV caps it. Both
/// have to rise together: a bigger cap alone changes nothing when the aim stays
/// where it was. Multiplying the configured bitrate for the duration moves the
/// pair, through the setBitrate() path every encoder already implements —
/// no new vendor code, no second rate-control mode to keep in step.
///
/// ── Three, not six, and why the number is a latency ─────────────────────────
///
/// The multiplier is not "how sharp": a still picture converges to the same
/// total number of bits whatever the per-frame cap, because every pass codes
/// the residual of the previous one — a smaller cap only spreads the same
/// bits over more passes. What the multiplier IS, is the longest the link can
/// be occupied by one pass, hence the longest a motion frame arriving just
/// after it can wait behind it: `boost / 60` seconds at the stream's own rate.
/// Six was 100 ms, and it was measured — on the bench a 20 Mbps stream's
/// first pass came out at 248 KB, exactly the ×6 cap, which is 100 ms of a
/// 20 Mbps link. Three is 50 ms, the price of one refinement pass paid by a
/// mouse that moves at the wrong moment; the passes are paced to the link by
/// LinkOccupancy below, so that price is paid at most once. The ceiling keeps
/// a burst on a very high bitrate stream from turning into a frame no link can
/// move in reasonable time.
constexpr int kStillBoost = 3;
constexpr int kStillMaxKbps = 500000;

inline int stillBitrateKbps(int streamKbps)
{
    if (streamKbps <= 0) return streamKbps;
    const int64_t boosted = static_cast<int64_t>(streamKbps) * kStillBoost;
    return static_cast<int>(boosted > kStillMaxKbps ? kStillMaxKbps : boosted);
}

/// How long the link is still busy with what has already been handed to it,
/// estimated from the stream's own bitrate.
///
/// ── Why an estimate, and not the transport's own figure ─────────────────────
///
/// The transport does report a `bufferedAmount`, and it is the obvious thing
/// to read. It is also blind exactly where it matters: it counts what
/// libdatachannel holds AFTER usrsctp refused it, and usrsctp's send buffer is
/// one mebibyte. On a 20 Mbps link that is 420 ms of backlog that reads as
/// zero. A refinement burst is a few hundred kilobytes; the figure that is
/// meant to say whether the link has drained it never moves.
///
/// What IS known is the bitrate the viewer set, which is their own statement
/// of what the link carries — the same number the rate control already trusts
/// for every frame. So the link is modelled as a pipe at that rate: each frame
/// handed over occupies it for size / rate, back to back, and the pipe is
/// drained when that time has passed. Motion frames keep it about level (a CBR
/// frame is one interval of link time, sent once per interval); what it exists
/// for is the still screen, where the refinement burst may send its next pass
/// only once the previous one has left. The burst then never runs ahead of the
/// link by more than one pass, whatever the stream's bitrate — instead of the
/// whole burst at once, which on a slow link is the whole burst in front of the
/// first frame of the next mouse movement.
///
/// A client-fed measure (the receiver's own arrival rate, §9.3) will replace
/// the bitrate as the rate here when it exists; the model does not change.
struct LinkOccupancy
{
    int64_t busyUntilUs = 0;

    /// `bytes` were handed to the link at `nowUs`, on a stream of `kbps`.
    void sent(int64_t nowUs, size_t bytes, int kbps)
    {
        if (kbps <= 0) return;
        const int64_t from = busyUntilUs > nowUs ? busyUntilUs : nowUs;
        busyUntilUs = from + static_cast<int64_t>(bytes) * 8000 / kbps;
    }

    /// Microseconds of link time still owed at `nowUs`; 0 once drained.
    int64_t backlogUs(int64_t nowUs) const { return busyUntilUs > nowUs ? busyUntilUs - nowUs : 0; }

    bool drainedAt(int64_t nowUs) const { return nowUs >= busyUntilUs; }
};

/// When a still picture has stopped improving — the refinement burst's exit.
///
/// ── Why size alone was the wrong witness ────────────────────────────────────
///
/// The burst used to stop when a pass came out under 2 KB: "the encoder had
/// nothing left to add". True at 1 Mbps, where that is what a pass of nothing
/// costs. False everywhere else: constant bitrate does not encode what the
/// picture needs, it encodes what the rate asks for, and pads or lowers the QP
/// until it gets there. Measured on a 55 Mbps stream at 165 fps: 26 passes of
/// ~120 KB each on a picture that had not changed — 3 MB over the whole
/// window, for every pause of the mouse, and the criterion never fired once.
///
/// So two witnesses, either one enough, and a ceiling behind both:
///
///  - a tiny pass — still right where it applies;
///  - a QP that no longer falls. Every pass codes the residual of the last,
///    so as long as the picture sharpens the QP the encoder reports for it
///    drops; when the budget outruns the residual the QP stops moving (or
///    rises, as the rate control hunts) and the extra bits are padding. Two
///    flat passes in a row, after enough passes to know the burst has really
///    begun. An encoder that reports no QP (-1: AMF on some drivers) simply
///    never satisfies this one;
///  - a hard cap on passes, for the encoder that reports nothing and a picture
///    that never gets tiny. Eight: at the ×3 cap a pass is 50 ms of link, so
///    the cap is at most 400 ms of link spent on one still picture.
///
/// The QP comparison is direction-only, so H.264/HEVC (0–51) and AV1's q-index
/// (0–255) are handled alike without knowing which is in use.
struct RefineConvergence
{
    static constexpr int kMinPasses = 4;
    static constexpr int kMaxPasses = 8;
    static constexpr size_t kQuietBytes = 2048;
    static constexpr int kQuietPasses = 2;

    enum class Verdict
    {
        Continue,
        Converged,
        Capped
    };

    int passes = 0;
    int quiet = 0;
    int lastQp = -1;

    /// One more pass went out, of `bytes`, at `avgQp` (or -1 when unknown).
    Verdict notePass(size_t bytes, int avgQp)
    {
        passes++;
        const bool tiny = bytes <= kQuietBytes;
        const bool qpFlat = avgQp >= 0 && lastQp >= 0 && avgQp >= lastQp;
        lastQp = avgQp;
        quiet = (passes >= kMinPasses && (tiny || qpFlat)) ? quiet + 1 : 0;
        if (quiet >= kQuietPasses) return Verdict::Converged;
        if (passes >= kMaxPasses) return Verdict::Capped;
        return Verdict::Continue;
    }

    void reset() { *this = RefineConvergence{}; }
};

} // namespace mw::native::encode
