#!/usr/bin/env python3
"""Sound - flag offset from mw-click-sound's streamed onsets (--tick runs).

usage: sound_offset.py <listener output> [--before MS] [--after MS]

mw-click-sound --tick prints every onset as it comes, `beep <us>` and
`flag <us>`, both on the client's QPC clock. Its own summary pairs each beep
with the nearest flag within 250 ms, which a grown jitter buffer outruns: on a
loaded Wi-Fi the sound came 300-500 ms after its flag (05/10). This pairs each
flag with the first beep from --before ms ahead of it to --after ms behind it
(default -100 / +900; clicks come about a second apart), one beep per flag,
and prints the offsets: positive = the sound after the picture.

Where the listener logged them (10/10), `gap <us> <len>` (no loopback packet:
no stream rendering) and `silent <us> <len>` (packets the mixer flagged
silent): each flag without a beep is placed at its flag + the median offset,
and counted as fallen in a gap, in a silent run, or in sound the browser
played as silence (A1: where the missing beeps go).
"""
import sys


def main():
    path = sys.argv[1]
    args = sys.argv[2:]
    before = float(args[args.index("--before") + 1]) if "--before" in args else 100.0
    after = float(args[args.index("--after") + 1]) if "--after" in args else 900.0
    beeps, flags, gaps, silents = [], [], [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3 and parts[0] in ("gap", "silent") and all(p.isdigit() for p in parts[1:]):
                (gaps if parts[0] == "gap" else silents).append((int(parts[1]), int(parts[2])))
                continue
            if len(parts) == 2 and parts[0] in ("beep", "flag") and parts[1].lstrip("-").isdigit():
                (beeps if parts[0] == "beep" else flags).append(int(parts[1]))
    beeps.sort()
    flags.sort()
    used = set()
    offsets = []
    lone = []
    for fl in flags:
        for i, b in enumerate(beeps):
            if i in used:
                continue
            d = (b - fl) / 1000.0
            if -before <= d <= after:
                used.add(i)
                offsets.append(d)
                break
        else:
            lone.append(fl)
    print("%d flags, %d beeps, %d paired (%d flags without a beep, %d beeps without a flag)" % (
        len(flags), len(beeps), len(offsets), len(flags) - len(offsets), len(beeps) - len(offsets)))
    if offsets:
        o = sorted(offsets)
        q = lambda p: o[min(len(o) - 1, int(p * (len(o) - 1) + 0.5))]
        print("sound - flag  median %.1f  p10 %.1f  p90 %.1f  min %.1f  max %.1f ms" % (
            q(0.5), q(0.1), q(0.9), o[0], o[-1]))
    if lone and offsets and (gaps or silents):
        median_us = int(sorted(offsets)[len(offsets) // 2] * 1000)
        # The beep's 20 ms, where it should have played.
        within = lambda t, spans: any(s0 <= t + 20000 and t <= s0 + n for s0, n in spans)
        in_gap = sum(1 for fl in lone if within(fl + median_us, gaps))
        in_silent = sum(1 for fl in lone if not within(fl + median_us, gaps)
                        and within(fl + median_us, silents))
        print("missing beeps: %d in a loopback gap, %d in a silent run, %d in sound played as silence"
              % (in_gap, in_silent, len(lone) - in_gap - in_silent))
    if gaps or silents:
        print("loopback: %d gaps (%.0f ms), %d silent runs (%.0f ms)" % (
            len(gaps), sum(n for _, n in gaps) / 1000.0, len(silents), sum(n for _, n in silents) / 1000.0))


if __name__ == "__main__":
    main()
