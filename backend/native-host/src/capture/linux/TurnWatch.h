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

#include "CompositorTransform.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// Whether the scanout's display was turned during a KMS capture (issue #54 on
// Linux): asked of the compositor on a thread of its own, every few hundred
// milliseconds, and handed to the capture loop as a flag.
//
// A turn keeps the display's mode and its buffer's shape, so the scanout says
// nothing of it; only the compositor does, and asking is a round trip to it.
// The capture loop used to ask on its still moments only, so as not to make a
// frame wait behind the answer — and a desktop that never stood still (a game,
// a video) never had its turn seen: the stream stayed on its side to the end
// (UM790Pro bench, 12/11/2026). Here the loop only reads an atomic flag.
//
// The thread reads with a function that holds copies, nothing of the session,
// so a compositor that never answers cannot hold the session's stop: the
// watch waits a moment for its thread and then leaves it to finish alone.
//
// Portable (std::thread only): tested on every OS with a stand-in reader.

namespace mw::native::capture {

class TurnWatch
{
public:
    using Reader = std::function<CompositorTransform()>;

    /// Starts watching: @p read is called every @p intervalMs until a reading
    /// that is known and differs from @p expected, which is then kept for
    /// take(). Readings after that one are not made: the capture starts over,
    /// and a new watch with it.
    TurnWatch(Reader read, Orientation expected, int intervalMs, int stopBudgetMs = 1000)
        : m_State(std::make_shared<State>())
        , m_StopBudgetMs(stopBudgetMs)
    {
        std::shared_ptr<State> state = m_State;
        std::thread([state, read = std::move(read), expected, intervalMs] {
            run(*state, read, expected, intervalMs);
        }).detach();
    }

    ~TurnWatch()
    {
        std::unique_lock<std::mutex> lock(m_State->mutex);
        m_State->stop = true;
        m_State->wake.notify_all();
        // A read in flight finishes first; one that hangs on a compositor
        // that does not answer is left behind, holding only its own state.
        m_State->wake.wait_for(lock, std::chrono::milliseconds(m_StopBudgetMs),
                               [this] { return m_State->finished; });
    }

    TurnWatch(const TurnWatch&) = delete;
    TurnWatch& operator=(const TurnWatch&) = delete;

    /// The turn seen, once: true and @p seen filled the first time it is asked
    /// after the thread saw it, false before and after. Never waits on the
    /// compositor.
    bool take(CompositorTransform& seen)
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        if (!m_State->turned || m_State->taken) return false;
        m_State->taken = true;
        seen = m_State->seen;
        return true;
    }

    /// How many readings the thread made, for the tests.
    int readings() const
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        return m_State->readings;
    }

private:
    struct State
    {
        std::mutex mutex;
        std::condition_variable wake;
        bool stop = false;
        bool finished = false;
        bool turned = false;
        bool taken = false;
        int readings = 0;
        CompositorTransform seen;
    };

    static void run(State& state, const Reader& read, Orientation expected, int intervalMs)
    {
        const auto interval = std::chrono::milliseconds(intervalMs > 0 ? intervalMs : 1);
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                if (state.wake.wait_for(lock, interval, [&] { return state.stop; })) break;
            }
            // Outside the lock: the round trip to the compositor.
            const CompositorTransform now = read();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.readings++;
            if (state.stop) break;
            if (!now.known || now.orientation == expected) continue;
            state.seen = now;
            state.turned = true;
            break;
        }
        std::lock_guard<std::mutex> lock(state.mutex);
        state.finished = true;
        state.wake.notify_all();
    }

    std::shared_ptr<State> m_State;
    int m_StopBudgetMs;
};

} // namespace mw::native::capture
