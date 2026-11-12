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

#include <mutex>

// One lock for every call into libX11 in the process.
//
// Xlib is not initialised for threads here (no XInitThreads: the library is
// loaded on demand, by whichever module needs it first, too late to promise
// it was called before any other), and its error handlers are the process's.
// Each module opens a connection of its own, but they run on threads of their
// own too — X11Layout read by a capture loop and by its turn watch
// (TurnWatch.h), X11Damage's and XFixesCursor's readers, X11Pointer under the
// input's lock — and a handler X11Layout put in place for its read would take
// another module's errors. So every one of them calls Xlib under this lock.
//
// Order: a module's own lock first, then this one, never the other way round.
// Xlib calls back into the process from inside a call — XFixesCursor's IO
// error handler, which takes its g_ioMutex — so that lock is taken under this
// one, and is never held while this one is asked for. No callback of the
// process's (a session's, the log's) is ever made under it.
//
// Leaked on purpose: a turn watch's thread left behind by a compositor that did
// not answer may still be in a read as the process exits, and must not find
// the lock destroyed under it.

namespace mw::native::platform {

inline std::mutex& xlibMutex()
{
    static std::mutex& lock = *new std::mutex;
    return lock;
}

} // namespace mw::native::platform
