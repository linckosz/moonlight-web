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

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

class ChipSong;
class MusicWorker;

/// Plays ChipSong on the default audio output, from a thread of its own.
///
/// The render loop keeps the GUI thread busy submitting frames, and a starved
/// audio buffer crackles: that crackle would then be heard on the client and
/// blamed on the stream. Here the audio never waits for the GPU, and every
/// buffer the device found empty is counted (`underruns`), so a crackle heard
/// on the client with 0 underruns on the host came from the way there.
class MusicPlayer : public QObject
{
    Q_OBJECT

public:
    MusicPlayer();
    ~MusicPlayer() override;

    /// Plays the tune from its first bar.
    void start();
    void stop();
    void blip(int direction);

    /// The kick's pulse for what the speakers play right now (0..1).
    float kickPulse() const;

    struct Stats
    {
        bool playing = false;
        int underruns = 0;
        /// Written but still queued in the device's buffer: the host's own audio
        /// latency, less the audio engine's last period.
        double bufferedMs = 0.0;
        QString error;
    };
    Stats stats() const;

private:
    friend class MusicWorker;

    QThread m_thread;
    MusicWorker* m_worker = nullptr;
    QElapsedTimer m_clock;

    // Written by the audio thread, read by the GUI thread.
    std::atomic<bool> m_playing{false};
    std::atomic<int> m_underruns{0};
    std::atomic<qint64> m_generated{0};    // frames written to the device
    std::atomic<qint64> m_heardFrames{-1}; // written minus still queued
    std::atomic<qint64> m_heardAtNs{0};    // m_clock when it was measured
    mutable QMutex m_errorLock;
    QString m_error;
    // The GUI thread's handle on the tune being played, for blip().
    std::shared_ptr<ChipSong> m_song;
};
