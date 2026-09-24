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

#include "MusicPlayer.h"

#include "ChipSong.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>

namespace {

constexpr int kBytesPerFrame = 4; // stereo, 16 bits
// The device's buffer. Long enough that a busy machine does not starve it, short
// enough that an arrow key's sweep follows the key.
constexpr int kBufferMs = 60;
constexpr int kPollMs = 5;

} // namespace

/// Lives in the player's thread and keeps the device's buffer full ("push"
/// mode). Pulling was tried first: Qt then reads the synth from a thread of its
/// own and reports a play position (processedUSecs) that drifted by 24 ms a
/// second and jumped back, so neither the picture's beat nor an underrun count could be
/// built on it (22/09/2026). Pushing, the buffer's fill level says both.
class MusicWorker : public QObject
{
public:
    explicit MusicWorker(MusicPlayer* player)
        : m_player(player)
    {}

    void start(std::shared_ptr<ChipSong> song)
    {
        stop();
        MusicPlayer* p = m_player;
        p->m_generated = 0;
        p->m_heardFrames = -1;
        p->m_underruns = 0;
        setError(QString());

        QAudioFormat format;
        format.setSampleRate(ChipSong::kRate);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            setError(QStringLiteral("no audio output"));
            return;
        }
        if (!device.isFormatSupported(format)) {
            setError(QString("%1 refuses 48 kHz stereo 16-bit").arg(device.description()));
            return;
        }
        m_song = std::move(song);
        m_pending.clear();
        m_sink = std::make_unique<QAudioSink>(device, format);
        m_sink->setBufferSize(ChipSong::kRate * kBytesPerFrame * kBufferMs / 1000);
        QObject::connect(m_sink.get(), &QAudioSink::stateChanged, this, [this](QAudio::State s) {
            if (!m_sink) return;
            if (s == QAudio::IdleState && m_sink->error() == QAudio::UnderrunError)
                m_starved = true;
            else if (s == QAudio::StoppedState && m_sink->error() != QAudio::NoError)
                setError(QString("audio stopped (error %1)").arg(int(m_sink->error())));
        });
        m_io = m_sink->start();
        if (!m_io) {
            setError(QString("%1 would not open").arg(device.description()));
            m_sink.reset();
            return;
        }
        m_primed = false;
        m_starved = false;
        feed();
        m_poll = std::make_unique<QTimer>();
        m_poll->setTimerType(Qt::PreciseTimer);
        m_poll->setInterval(kPollMs);
        QObject::connect(m_poll.get(), &QTimer::timeout, this, [this]() { feed(); });
        m_poll->start();
        p->m_playing = true;
    }

    void stop()
    {
        m_player->m_playing = false;
        m_poll.reset();
        if (m_sink) m_sink->stop();
        m_io = nullptr;
        m_sink.reset();
        m_song.reset();
    }

private:
    void setError(const QString& text)
    {
        QMutexLocker lock(&m_player->m_errorLock);
        m_player->m_error = text;
    }

    void feed()
    {
        if (!m_sink || !m_io) return;
        const qint64 size = m_sink->bufferSize();
        // An empty buffer, once it has been full, is audio the device had to
        // make up: a click on the host itself, before any stream.
        const bool dry = m_primed && m_sink->bytesFree() >= size;
        if (dry || m_starved) ++m_player->m_underruns;
        m_starved = false;

        const qint64 free = m_sink->bytesFree();
        if (m_pending.isEmpty() && free >= kBytesPerFrame) {
            const int frames = int(free / kBytesPerFrame);
            m_pending.resize(qsizetype(frames) * kBytesPerFrame);
            m_song->render(reinterpret_cast<qint16*>(m_pending.data()), frames);
        }
        if (!m_pending.isEmpty()) {
            const qint64 written = m_io->write(m_pending);
            if (written > 0) {
                m_player->m_generated += written / kBytesPerFrame;
                m_pending.remove(0, written);
                m_primed = true;
            }
        }
        const qint64 queued = (size - m_sink->bytesFree()) / kBytesPerFrame;
        m_player->m_heardFrames = std::max<qint64>(0, m_player->m_generated - queued);
        m_player->m_heardAtNs = m_player->m_clock.nsecsElapsed();
    }

    MusicPlayer* m_player;
    std::shared_ptr<ChipSong> m_song;
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice* m_io = nullptr;
    std::unique_ptr<QTimer> m_poll;
    QByteArray m_pending; // rendered, not yet taken by the device
    bool m_primed = false;
    bool m_starved = false;
};

MusicPlayer::MusicPlayer()
{
    m_clock.start();
    m_worker = new MusicWorker(this);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start(QThread::TimeCriticalPriority);
}

MusicPlayer::~MusicPlayer()
{
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker]() { w->stop(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void MusicPlayer::start()
{
    m_song = std::make_shared<ChipSong>();
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker, song = m_song]() { w->start(song); }, Qt::QueuedConnection);
}

void MusicPlayer::stop()
{
    m_playing = false;
    QMetaObject::invokeMethod(m_worker, [w = m_worker]() { w->stop(); }, Qt::QueuedConnection);
}

void MusicPlayer::blip(int direction)
{
    if (m_song && m_playing) m_song->blip(direction);
}

float MusicPlayer::kickPulse() const
{
    const qint64 heard = m_heardFrames;
    if (!m_playing || heard < 0) return 0.0f;
    // Carry the last measure forward on the wall clock, never by more than a
    // few polls.
    const qint64 sinceNs = std::min<qint64>(m_clock.nsecsElapsed() - m_heardAtNs, 20'000'000);
    return ChipSong::kickPulse(heard + sinceNs * ChipSong::kRate / 1'000'000'000);
}

MusicPlayer::Stats MusicPlayer::stats() const
{
    Stats s;
    s.playing = m_playing;
    s.underruns = m_underruns;
    const qint64 heard = m_heardFrames;
    if (heard >= 0) s.bufferedMs = (m_generated - heard) * 1000.0 / ChipSong::kRate;
    QMutexLocker lock(&m_errorLock);
    s.error = m_error;
    return s;
}
