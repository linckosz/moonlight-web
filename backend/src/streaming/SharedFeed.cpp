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

#include "SharedFeed.h"
#include "FeedPublisher.h"

#include "../backend/streambackend/NativeHostBackend.h"
#include "../server/GuestProfile.h"

#include <QDebug>
#include <QJsonObject>

SharedFeed::SharedFeed(QString edition, QObject* parent)
    : QObject(parent)
    , m_Edition(std::move(edition))
{
    m_IdleTimer.setSingleShot(true);
    connect(&m_IdleTimer, &QTimer::timeout, this, [this]() {
        if (!m_Guests.isEmpty()) return;
        qInfo() << "[SharedFeed] no guest for" << kIdleStopMs / 1000 << "s — the feed stops";
        stopWorker();
    });
    m_RestartTimer.setSingleShot(true);
    connect(&m_RestartTimer, &QTimer::timeout, this, [this]() {
        if (m_Guests.isEmpty() || m_Worker) return;
        launch(true);
    });
    m_AutoTimer.setSingleShot(true);
    connect(&m_AutoTimer, &QTimer::timeout, this, [this]() { reconsider(); });
}

SharedFeed::~SharedFeed()
{
    stopWorker();
}

bool SharedFeed::enabled(bool setting)
{
#ifdef Q_OS_WIN
    return setting && qEnvironmentVariable("MW_SHARED_FEED") != QLatin1String("0");
#else
    // The guest's session without a capture exists on Windows only (S2):
    // elsewhere every guest keeps an encoder of its own.
    Q_UNUSED(setting);
    return false;
#endif
}

bool SharedFeed::acquire(const Spec& spec, int slot, Ticket& out)
{
    if (m_Failed) return false;
    // Its wish, not yet weighed: only once its browser has the stream (G1).
    const auto noteWant = [this, &spec, slot]() {
        m_Wants.insert(slot, Want{spec.wantHeight, spec.lan, false});
    };
    // A relaunch is due under the same pipe, and the guests on the feed wait
    // for it there. One arriving meanwhile waits with them when it wants that
    // feed as it will come back; any other encodes on its own rather than
    // start a second feed, or pull the pipe from under the waiting ones.
    if (!m_Worker && relaunchDue()) {
        if (!spec.sameFeed(m_Spec) || (spec.h264 && !m_Spec.h264)) {
            qInfo() << "[SharedFeed] slot" << slot
                    << "arrives while the feed relaunches, wanting another one — this guest "
                       "encodes on its own";
            return false;
        }
        m_Guests.insert(slot);
        noteWant();
        out.pipe = m_Pipe;
        out.token = m_Token;
        out.launch = m_Launch;
        return true;
    }
    QSet<int> others = m_Guests;
    others.remove(slot);
    if (m_Worker && !spec.sameFeed(m_Spec)) {
        // The owner moved to another display: a feed with nobody on it is
        // replaced, one with guests is theirs, and this guest encodes on its
        // own.
        if (!others.isEmpty()) {
            qInfo() << "[SharedFeed] slot" << slot << "wants app" << spec.appId << "at"
                    << spec.height << "— the feed shows app" << m_Spec.appId << "at"
                    << m_Spec.height << "to other guests: this guest encodes on its own";
            return false;
        }
        stopWorker();
    } else if (m_Worker && spec.h264 && !m_Spec.h264) {
        // A guest whose browser decodes no HEVC: the whole feed goes H.264,
        // which every browser decodes, and stays there until it stops — never
        // two feeds. The guests already on it are told by the feed itself;
        // their pages come back the way a codec fallback does, into the new
        // one. One keyframe for everyone, once.
        if (!others.isEmpty()) {
            qInfo() << "[SharedFeed] slot" << slot << "decodes no HEVC — the feed goes H.264,"
                    << others.size() << "guest(s) on it rejoin";
            retireForCodec(QStringLiteral("h264"));
        } else {
            stopWorker();
        }
    }
    m_IdleTimer.stop();
    if (!m_Worker) {
        // An H.264 feed serves every browser (a guest that decodes HEVC joins
        // it as it is, above): once switched, it stays so until it stops.
        m_Spec = spec;
        m_Pipe = feedpipe::newName(m_Edition);
        m_Token = feedpipe::newToken();
        m_Failures = 0;
        // A new feed in Auto starts at its first guest's height, the first
        // rebuild free of the minute's wait.
        m_LastAutoRebuild.invalidate();
        m_AutoTimer.stop();
        if (!launch(false)) return false;
    }
    m_Guests.insert(slot);
    noteWant();
    out.pipe = m_Pipe;
    out.token = m_Token;
    out.launch = m_Launch;
    return true;
}

void SharedFeed::release(int slot)
{
    m_Wants.remove(slot);
    if (!m_Guests.remove(slot)) return;
    // Auto does not go down for a guest leaving (I67-baisse B): the feed keeps
    // its height until the last one goes, and then stops.
    if (m_Guests.isEmpty()) m_AutoTimer.stop();
    if (m_Guests.isEmpty() && m_Worker) m_IdleTimer.start(kIdleStopMs);
}

void SharedFeed::guestConnected(int slot)
{
    auto it = m_Wants.find(slot);
    if (it == m_Wants.end() || !m_Guests.contains(slot) || it->connected) return;
    it->connected = true;
    reconsider();
}

void SharedFeed::reset()
{
    m_Guests.clear();
    m_Wants.clear();
    m_Resizing = false;
    m_IdleTimer.stop();
    m_RestartTimer.stop();
    m_AutoTimer.stop();
    m_LastAutoRebuild.invalidate();
    m_Failed = false;
    m_Failures = 0;
    stopWorker();
}

bool SharedFeed::launch(bool relaunch)
{
    QJsonObject cfg;
    cfg[QStringLiteral("role")] = QStringLiteral("feed");
    cfg[QStringLiteral("backendType")] = QStringLiteral("native");
    cfg[QStringLiteral("hostUuid")] = m_Spec.hostUuid;
    cfg[QStringLiteral("appId")] = m_Spec.appId;
    // The display by its key, resolved here: the worker does not read the
    // settings file the display cards are numbered in.
    QString why;
    cfg[QStringLiteral("displayKey")] = NativeHostBackend::displayKeyForApp(m_Spec.appId, &why);
    if (!why.isEmpty())
        qWarning() << "[SharedFeed] no display for app" << m_Spec.appId << ":" << why;
    cfg[QStringLiteral("width")] = m_Spec.width;
    cfg[QStringLiteral("height")] = m_Spec.height;
    cfg[QStringLiteral("fps")] = 60;
    cfg[QStringLiteral("bitrateKbps")] = m_Spec.bitrateKbps;
    cfg[QStringLiteral("h264")] = m_Spec.h264;
    cfg[QStringLiteral("governorFloorPercent")] = 60;
    cfg[QStringLiteral("feedPipe")] = m_Pipe;
    cfg[QStringLiteral("feedToken")] = QString::fromLatin1(m_Token);
    cfg[QStringLiteral("nativeVideoPipeline")] = m_Spec.videoPipeline;
    cfg[QStringLiteral("nativeTuning")] = m_Spec.tuning;
    cfg[QStringLiteral("portalRestoreToken")] = m_Spec.portalToken;

    auto* worker = new StreamWorkerHost(this);
    connect(worker, &StreamWorkerHost::exited, worker, &QObject::deleteLater);
    connect(worker, &StreamWorkerHost::responseReady, this,
            [this, worker](int code, const QJsonObject& body) {
                if (worker != m_Worker) return;
                if (code == 200)
                    qInfo().noquote()
                        << "[SharedFeed] feed up:" << body.value(QStringLiteral("feed")).toString();
                else
                    qWarning() << "[SharedFeed] the feed did not start (" << code
                               << "):" << body.value(QStringLiteral("error")).toString();
            });
    connect(worker, &StreamWorkerHost::ended, this, [this, worker]() { onEnded(worker); });
    // A relaunch runs exactly as the feed its guests are waiting for did: the
    // pipe answers to that user only.
    const bool started = relaunch ? worker->startAs(cfg, m_Launch) : worker->start(cfg);
    if (!started) {
        qWarning() << "[SharedFeed] the feed's worker could not be started";
        worker->deleteLater();
        return false;
    }
    m_Worker = worker;
    m_Launch = worker->launch();
    m_RunningFor.start();
    qInfo() << "[SharedFeed]" << (relaunch ? "relaunched" : "launched") << "the feed of app"
            << m_Spec.appId << "at" << m_Spec.width << "x" << m_Spec.height
            << (m_Spec.h264 ? "H.264" : "HEVC") << m_Spec.bitrateKbps << "kbps on" << m_Pipe;
    return true;
}

void SharedFeed::resize(int height)
{
    if (height < 0) return;
    if (height == 0) {
        // Auto: the feed goes where its connected guests' screens want it.
        if (m_Spec.autoHeight) return;
        m_Spec.autoHeight = true;
        reconsider(true);
        return;
    }
    if (height == m_Spec.height && !m_Spec.autoHeight) return;
    // A rung of the owner's beats Auto, whatever the guests' screens (G3).
    m_Spec.autoHeight = false;
    m_AutoTimer.stop();
    // The same shape, and the rate the guest profile gives a rung (main.cpp:
    // the standard estimate for the height, halved).
    const int was = m_Spec.height;
    m_Spec.height = height;
    m_Spec.width = static_cast<int>(height * m_Spec.aspectRatio + 0.5) & ~1;
    m_Spec.bitrateKbps = qMax(
        1000, qRound(20000.0 * (static_cast<double>(height) * height) / (1080.0 * 1080.0)) / 2);
    if (height == was) {
        // Back from Auto at the same height: only the rate changes, and it
        // is not worth a cut for the guests watching; the next launch takes it.
        return;
    }
    qInfo() << "[SharedFeed] the owner picked" << height << "p";
    rebuild("the owner's pick");
}

void SharedFeed::reconsider(bool ownerPick)
{
    m_AutoTimer.stop();
    if (!m_Spec.autoHeight || m_Failed) return;
    QList<mw::guestprofile::FeedGuest> guests;
    for (const int slot : m_Guests) {
        const Want w = m_Wants.value(slot);
        guests.append({w.height, w.lan, w.connected});
    }
    const mw::guestprofile::FeedTarget now{m_Spec.height, m_Spec.lan, 0};
    const mw::guestprofile::FeedTarget target =
        mw::guestprofile::feedAutoTarget(now, guests, m_Spec.hostScreenHeight, ownerPick);
    const int kbps =
        mw::guestprofile::autoBitrateKbps(target.height, m_Spec.aspectRatio, target.lan);
    if (target.height == m_Spec.height && target.lan == m_Spec.lan && kbps == m_Spec.bitrateKbps)
        return;
    // Not running, or a relaunch already due: that one takes the new spec.
    const bool live = m_Worker && !relaunchDue();
    // One rebuild a minute at most for the whole feed (G2); the owner's own
    // pick of Auto does not wait.
    if (live && !ownerPick && m_LastAutoRebuild.isValid() &&
        m_LastAutoRebuild.elapsed() < mw::guestprofile::kFeedAutoRebuildGapMs) {
        m_AutoTimer.start(static_cast<int>(mw::guestprofile::kFeedAutoRebuildGapMs -
                                           m_LastAutoRebuild.elapsed()));
        return;
    }
    m_Spec.height = target.height;
    m_Spec.width = static_cast<int>(target.height * m_Spec.aspectRatio + 0.5) & ~1;
    m_Spec.lan = target.lan;
    m_Spec.bitrateKbps = kbps;
    // The height and how many guests decided it, nothing about them (G3).
    qInfo() << "[SharedFeed] Auto:" << target.height << "lines for" << target.counted << "guest(s)";
    if (!live) return;
    m_LastAutoRebuild.start();
    rebuild("Auto");
}

void SharedFeed::rebuild(const char* why)
{
    // Not running, or a death's relaunch already due: that one takes the new
    // spec. A feed nobody watches stops; the next guest starts it anew.
    if (!m_Worker) return;
    if (m_Guests.isEmpty()) {
        stopWorker();
        return;
    }
    qInfo() << "[SharedFeed]" << why << "— the feed is rebuilt at" << m_Spec.width << "x"
            << m_Spec.height << "under the same pipe, its" << m_Guests.size() << "guest(s) follow";
    StreamWorkerHost* worker = m_Worker.data();
    m_Worker.clear();
    m_IdleTimer.stop();
    // Its end is asked for: not a death to count.
    disconnect(worker, &StreamWorkerHost::ended, this, nullptr);
    m_Resizing = true;
    // The pipe is the old worker's name until it is gone.
    connect(worker, &StreamWorkerHost::exited, this, [this]() {
        m_Resizing = false;
        if (!m_Worker && !m_Guests.isEmpty() && !m_Failed) launch(true);
    });
    worker->requestQuit();
}

void SharedFeed::retireForCodec(const QString& codec)
{
    StreamWorkerHost* worker = m_Worker.data();
    m_Worker.clear();
    m_IdleTimer.stop();
    m_RestartTimer.stop();
    // Its guests leave it; they come back to the new feed, one by one.
    m_Guests.clear();
    if (!worker) return;
    // Its end is asked for: not a death to count.
    disconnect(worker, &StreamWorkerHost::ended, this, nullptr);
    worker->sendControl(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("codec")},
                                    {QStringLiteral("codec"), codec}});
    // It ends itself once its guests have read the news; one that does not
    // is ended.
    QPointer<StreamWorkerHost> guard(worker);
    QTimer::singleShot(3000, this, [guard]() {
        if (guard) guard->requestQuit();
    });
}

void SharedFeed::stopWorker()
{
    m_IdleTimer.stop();
    m_RestartTimer.stop();
    // A rebuild waiting for its minute dies with the feed.
    m_AutoTimer.stop();
    StreamWorkerHost* worker = m_Worker.data();
    m_Worker.clear();
    if (!worker) return;
    // Its end is the one asked for: not a death to count.
    disconnect(worker, &StreamWorkerHost::ended, this, nullptr);
    worker->requestQuit();
}

void SharedFeed::onEnded(StreamWorkerHost* worker)
{
    if (worker != m_Worker) return;
    m_Worker.clear();
    if (m_Guests.isEmpty()) return;
    // A feed that ran a good while before it died starts the count again: it
    // is a crash, not a feed that cannot run.
    if (m_RunningFor.isValid() && m_RunningFor.elapsed() >= kHealthyMs) m_Failures = 0;
    ++m_Failures;
    if (m_Failures >= kMaxFailures) {
        m_Failed = true;
        qWarning() << "[SharedFeed] the feed died" << m_Failures
                   << "times running — its guests rejoin on encoders of their own";
        const QSet<int> guestSlots = m_Guests;
        m_Guests.clear();
        emit failedForGood(guestSlots);
        return;
    }
    const int delayMs = 250 << (m_Failures - 1);
    qWarning() << "[SharedFeed] the feed died (" << m_Failures << "of" << kMaxFailures
               << ") — relaunched in" << delayMs << "ms under the same pipe";
    m_RestartTimer.start(delayMs);
}
