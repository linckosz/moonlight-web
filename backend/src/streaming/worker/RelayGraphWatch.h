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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

// When a stream worker told to stop may exit: once the relay graphs it let go
// are gone, all of them, and no later than a bound.
//
// A relay is deleted on its own session thread (spawnRelayThread), with its
// children: the signaling server, then the engine, whose destructor is where a
// Moonlight shim still waiting on LiStartConnection runs LiStopConnection. The
// relay's own pointer is cleared before its children go (Qt clears an object's
// guards first, then deletes its children), so it says nothing about the
// engine: the engine is watched too, and so is the thread, which quits once
// the relay is destroyed and so finishes only after every child is. What the
// graph leaves behind it outside Qt can be waited for as well (until()): the
// worker waits for libdatachannel, whose PeerConnections finish closing on
// threads of their own once the relay has let go of them.
//
// The worker used to wait a fixed 3 s instead. That was too long in the common
// case (the graph is down within tens of ms) and too short in the bad one: a
// teardown entered late, the main thread held up elsewhere for a few seconds
// (ScreenCaptureKit, 11/11), spent its 3 s whole and outlived the server's
// 5 s, which then killed it. The bound stays 3 s from the start of the
// teardown, so no exit comes later than it did.
//
// watch(), gone() and whenGone() run on the thread the watched QThread objects
// belong to (the worker's main thread, where spawnRelayThread makes them):
// they are deleted there, so reading them there is safe.
class RelayGraphWatch
{
public:
    /// The most a worker waits for its graph, from the start of its teardown.
    static constexpr int kBoundMs = 3000;
    static constexpr int kPollMs = 20;

    /// @p root is about to be deleted (deleteLater), @p engine with it. Call
    /// before deleteLater: the root's thread is read from the root.
    void watch(QObject* root, QObject* engine)
    {
        if (root) {
            m_Objects.emplace_back(root);
            QThread* t = root->thread();
            // A root left on this thread has no thread of its own to wait for.
            if (t && t != QThread::currentThread()) m_Threads.emplace_back(t);
        }
        if (engine) m_Objects.emplace_back(engine);
    }

    /// Something else to wait for, @p name for the log: gone once @p settled
    /// says so. Read on the watching thread, so it must be cheap and safe there.
    void until(const char* name, std::function<bool()> settled)
    {
        if (settled) m_Others.emplace_back(name, std::move(settled));
    }

    /// Everything watched is gone: the objects destroyed, their threads
    /// finished (or already deleted), the other conditions met. True when
    /// nothing was watched.
    bool gone() const { return left().isEmpty(); }

    /// What is still there, for the log ("1 object, 1 thread, libdatachannel");
    /// empty once gone.
    QString left() const
    {
        int objects = 0;
        for (const auto& o : m_Objects)
            if (!o.isNull()) ++objects;
        int threads = 0;
        for (const auto& t : m_Threads)
            if (t && !t->isFinished()) ++threads;
        QStringList parts;
        if (objects)
            parts << QString::number(objects) +
                         (objects > 1 ? QStringLiteral(" objects") : QStringLiteral(" object"));
        if (threads)
            parts << QString::number(threads) +
                         (threads > 1 ? QStringLiteral(" threads") : QStringLiteral(" thread"));
        for (const auto& other : m_Others)
            if (!other.second()) parts << QString::fromLatin1(other.first);
        return parts.join(QStringLiteral(", "));
    }

    bool empty() const { return m_Objects.empty() && m_Threads.empty() && m_Others.empty(); }

    /// Calls @p done once, on @p context's thread: as soon as everything
    /// watched is gone (inTime = true, @p left empty), or once @p boundMs have
    /// passed since @p since started (inTime = false, @p left says what is
    /// still there), whichever comes first. @p elapsedMs counts from @p since
    /// too. Nothing is called if @p context goes first.
    void
    whenGone(QObject* context, const QElapsedTimer& since, int boundMs,
             std::function<void(bool inTime, qint64 elapsedMs, const QString& left)> done) const
    {
        auto watch = std::make_shared<RelayGraphWatch>(*this);
        auto* timer = new QTimer(context);
        timer->setInterval(kPollMs);
        QObject::connect(timer, &QTimer::timeout, context,
                         [watch, timer, since, boundMs, done = std::move(done)]() {
                             const QString left = watch->left();
                             const qint64 elapsed = since.elapsed();
                             if (!left.isEmpty() && elapsed < boundMs) return;
                             timer->stop();
                             timer->deleteLater();
                             done(left.isEmpty(), elapsed, left);
                         });
        timer->start();
    }

private:
    std::vector<QPointer<QObject>> m_Objects;
    std::vector<QPointer<QThread>> m_Threads;
    std::vector<std::pair<const char*, std::function<bool()>>> m_Others;
};
