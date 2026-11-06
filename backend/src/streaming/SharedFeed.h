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

#include "StreamWorkerHost.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

/**
 * @brief The guests' shared feed of this machine's native host, from the
 * server's side (plan « flux commun des invités », S5).
 *
 * The owner keeps a stream of their own, untouched. Their guests — up to three
 * — used to get one each too: a capture and an encoder per guest, all encoding
 * the same picture at the same moment, the owner's encode waiting behind theirs
 * (bench §8q: on an RTX, the owner's host total doubles with three). Here they
 * share one: a `feed` worker captures and encodes the owner's display once, and
 * each guest's worker carries its pictures to that guest's browser (the pipe:
 * FeedPublisher / FeedSubscriber).
 *
 * This class decides when that worker runs: launched by the first guest's
 * join, stopped kIdleStopMs after the last one left (a guest back sooner costs
 * nothing), relaunched under the same pipe name when it dies — its guests
 * wait for it (FeedSubscriber::kRejoinMs) — and given up after kMaxFailures
 * deaths in a row: its guests then rejoin on encoders of their own.
 *
 * Main thread only.
 */
class SharedFeed : public QObject
{
    Q_OBJECT

public:
    /// What a feed shows and how. Two guests share one when it shows the same
    /// display at the same height; the codec is not a reason for two feeds
    /// (an H.264 one serves every browser).
    struct Spec
    {
        QString hostUuid;
        int appId = 0;
        int width = 0;
        int height = 1080;
        int bitrateKbps = 10000;
        /// This guest's browser decodes no HEVC.
        bool h264 = false;
        /// The machine's own, as every native session carries them.
        QString videoPipeline;
        QString tuning;
        QString portalToken;

        bool sameFeed(const Spec& o) const
        {
            return hostUuid == o.hostUuid && appId == o.appId && height == o.height;
        }
    };

    /// What a guest's worker needs to join the feed, and how it must be
    /// started to be let in (the pipe is its user's only).
    struct Ticket
    {
        QString pipe;
        QByteArray token;
        StreamWorkerHost::Launch launch = StreamWorkerHost::Launch::Plain;
    };

    static constexpr int kIdleStopMs = 10000;
    static constexpr int kMaxFailures = 3;
    /// A feed that ran this long before it died starts the count again.
    static constexpr qint64 kHealthyMs = 60000;

    /// @p edition names the pipes (feedpipe::newName): an instance's feed
    /// never meets another instance's.
    explicit SharedFeed(QString edition, QObject* parent = nullptr);
    ~SharedFeed() override;

    /// Whether the guests of this machine's native host share one feed: on
    /// Windows, with @p setting (AppSettings::sharedFeedEnabled) on and
    /// MW_SHARED_FEED not "0". Off, every guest encodes on its own, exactly
    /// as before.
    static bool enabled(bool setting);

    /// The feed for @p spec, for the guest of @p slot, launched now when none
    /// runs. False: this guest encodes on its own — another display's feed
    /// has guests on it, or this share's feed failed for good.
    bool acquire(const Spec& spec, int slot, Ticket& out);

    /// The guest of @p slot is gone (or never made it in). The last one
    /// leaving stops the feed after kIdleStopMs.
    void release(int slot);

    /// The owner stopped: the feed goes now, and the next share starts
    /// afresh (a feed given up on is tried again).
    void reset();

    /// The owner picked another height for the guests' picture. A feed with
    /// guests on it is rebuilt at it under the same pipe — they wait for it
    /// as for a relaunch after a death, and their decoders take the new size
    /// on its first keyframe — the width and the rate following, as the guest
    /// profile's do. An idle feed simply stops: the next guest starts it anew.
    void resize(int height);

    bool running() const { return !m_Worker.isNull(); }
    /// The host the current feed is of, empty when none was ever asked for.
    const QString& hostUuid() const { return m_Spec.hostUuid; }

signals:
    /// The feed died kMaxFailures times in a row: the guests of @p guestSlots must
    /// leave and rejoin, each on an encoder of its own.
    void failedForGood(const QSet<int>& guestSlots);

private:
    /// Start the worker for m_Spec on m_Pipe; @p how, the way the previous
    /// one ran, when it is a relaunch its guests are waiting for.
    bool launch(bool relaunch);
    /// A relaunch under the same pipe is on its way (a death's, or a new
    /// height's): the guests on the feed are waiting for it.
    bool relaunchDue() const { return m_RestartTimer.isActive() || m_Resizing; }
    void stopWorker();
    /// The feed switches codec: its worker tells its guests (their pages
    /// rejoin the way a codec fallback does) and ends itself.
    void retireForCodec(const QString& codec);
    void onEnded(StreamWorkerHost* worker);

    QString m_Edition;
    Spec m_Spec;
    QString m_Pipe;
    QByteArray m_Token;
    QPointer<StreamWorkerHost> m_Worker;
    StreamWorkerHost::Launch m_Launch = StreamWorkerHost::Launch::Plain;
    QSet<int> m_Guests;
    QTimer m_IdleTimer;
    QTimer m_RestartTimer;
    QElapsedTimer m_RunningFor;
    int m_Failures = 0;
    /// The feed is being rebuilt at a new height: the old worker is on its
    /// way out, the new one starts once it is gone (the pipe is its name).
    bool m_Resizing = false;
    /// Given up on for this share: its guests encode on their own until the
    /// owner stops (reset()).
    bool m_Failed = false;
};
