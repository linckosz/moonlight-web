/*
 * MoonlightWeb — native capture & encoding engine: GPU load tool.
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

#include <QtGlobal>

#include <atomic>
#include <vector>

/// "Neon Core", an original tune in the manner of the Atari ST: the three
/// square-wave voices of its YM2149 sound chip — a buzzer bass, 50 Hz chord
/// arpeggios, a pulse-width lead — plus synthesized "digidrums", the sampled
/// kit ST musicians played through a timer as a fourth voice.
///
/// Everything is computed, nothing is sampled: the tune is in the tables of
/// ChipSong.cpp. It is played by a 50 Hz tick, as an ST replay routine ran on
/// the vertical blank of a PAL screen: 6 ticks a row, 16 rows a bar, 125 BPM.
/// Its 31 bars last 59.5 s, so a 60 s run hears it once, from the intro to the
/// final chord.
///
/// It is also a test signal. Square waves are unforgiving: a dropped or
/// repeated buffer on the way to the client is a click nobody can miss. And the
/// kick drum is on every beat, so the picture can beat with it (kickPulse) and
/// show whether sound and image arrive together.
class ChipSong
{
public:
    static constexpr int kRate = 48000;
    static constexpr int kTickFrames = kRate / 50;
    static constexpr int kTicksPerRow = 6;
    static constexpr int kRowFrames = kTickFrames * kTicksPerRow;
    static constexpr int kRowsPerBar = 16;

    ChipSong();

    /// Fills `frames` interleaved stereo frames, from where the last call
    /// stopped. Called by the audio thread only.
    void render(qint16* out, int frames);
    /// A short sweep up (`direction` > 0) or down, mixed over the tune: the
    /// arrow keys sound, so an input is heard as well as seen. Any thread.
    void blip(int direction) { m_blip.store(direction > 0 ? 1 : -1); }

    static qint64 songFrames();
    /// How strong the picture's beat is at `frame` of the song (0..1): 1 when
    /// a kick hits, fading within a beat. A pure function of the score, so the
    /// render thread can ask it for the frame the speakers are playing now.
    static float kickPulse(qint64 frame);

private:
    struct Tone
    {
        double phase = 0.0;
        double freq = 0.0;
        double duty = 0.5;
        int volume = 0; // 0..15, the YM2149's logarithmic steps
    };

    void tick();
    float drums();

    qint64 m_tick = 0;
    int m_tickFrame = 0;

    // Channel A: the bass, a square gated by the YM's sawtooth envelope run at
    // audio rate and slightly detuned — the "buzzer" sound of the ST.
    Tone m_bass;
    double m_bassEnvPhase = 0.0;
    int m_bassGate = 0;
    // Channel B: chords as 50 Hz arpeggios.
    Tone m_arp;
    int m_arpRoot = -1;
    bool m_arpMinor = true;
    int m_arpAge = 99;
    // Channel C: the lead, pulse width swept like a "SID voice".
    Tone m_lead;
    int m_leadNote = -1;
    int m_leadAge = 0;
    bool m_leadHeld = false;
    std::vector<float> m_echo;
    size_t m_echoPos = 0;

    // Digidrums: seconds since each hit, negative when silent.
    double m_kickT = -1.0, m_kickPhase = 0.0;
    double m_snareT = -1.0, m_snarePhase = 0.0;
    double m_hatT = -1.0, m_openT = -1.0;
    float m_hatGain = 0.0f;
    quint32 m_lfsr = 1;
    float m_noise = 0.0f, m_noisePrev = 0.0f;
    double m_noiseClock = 0.0;

    // The arrow-key sweep.
    std::atomic<int> m_blip{0};
    double m_blipT = -1.0, m_blipPhase = 0.0;
    int m_blipDir = 1;

    // Output stage: one-pole low-pass (the ST's output filter) and DC block.
    float m_lp[2] = {0.0f, 0.0f};
    float m_dcIn[2] = {0.0f, 0.0f}, m_dcOut[2] = {0.0f, 0.0f};
};
