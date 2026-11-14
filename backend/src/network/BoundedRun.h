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

#include <QMetaObject>
#include <QObject>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace mw {

/// Hands @p job to whatever @p post sends it to (the thread that is to run
/// it) and waits at most @p budgetMs for it to end. True when it did.
///
/// When it did not, the job goes on over there: whatever it touches must
/// outlive it, which means the caller ends that thread before anything the
/// job reaches is destroyed. Nothing here cuts the job. Its end is told by a
/// standard mutex, so that what the job did is seen by whoever waited.
inline bool runWithin(const std::function<void(std::function<void()>)>& post,
                      std::function<void()> job, int budgetMs)
{
    struct State
    {
        std::mutex lock;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<State>();
    post([job = std::move(job), state]() {
        job();
        {
            std::lock_guard<std::mutex> guard(state->lock);
            state->done = true;
        }
        state->cv.notify_all();
    });
    std::unique_lock<std::mutex> guard(state->lock);
    return state->cv.wait_for(guard, std::chrono::milliseconds(budgetMs),
                              [&state]() { return state->done; });
}

/// The same on the thread @p context lives on, after what is queued there.
inline bool runWithin(QObject* context, std::function<void()> job, int budgetMs)
{
    return runWithin(
        [context](std::function<void()> f) {
            QMetaObject::invokeMethod(context, std::move(f), Qt::QueuedConnection);
        },
        std::move(job), budgetMs);
}

} // namespace mw
