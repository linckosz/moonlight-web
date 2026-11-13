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

#include "GnomeNaturalScroll.h"

#include "../../core/Log.h"
#include "../NaturalScroll.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mw::native::input {

namespace {

constexpr const char* kGsettings = "/usr/bin/gsettings";
constexpr auto kReadEvery = std::chrono::seconds(2);
constexpr int kTimeoutMs = 1000;

/// Run gsettings for the one key, @p kTimeoutMs at most. Its standard output
/// in @p out (a few bytes are all an answer takes), the exit code returned, or
/// -1 when it could not run or did not finish in time.
int askGsettings(std::string& out)
{
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) return -1;

    // The child gets /dev/null, the pipe and nothing else of ours.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);

    char arg0[] = "gsettings";
    char arg1[] = "get";
    char arg2[] = "org.gnome.desktop.peripherals.mouse";
    char arg3[] = "natural-scroll";
    char* argv[] = {arg0, arg1, arg2, arg3, nullptr};

    pid_t child = -1;
    const int rc = ::posix_spawn(&child, kGsettings, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        return -1;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) break;
        pollfd p{fds[0], POLLIN, 0};
        const int ready = ::poll(&p, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        char buffer[64];
        const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (out.size() < 256) out.append(buffer, static_cast<size_t>(n));
    }
    ::close(fds[0]);

    int status = 0;
    for (;;) {
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) return -1;
        if (Clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace

void GnomeNaturalScroll::start()
{
    if (m_Thread.joinable()) return;
    if (!desktopIsGnome(std::getenv("XDG_CURRENT_DESKTOP"))) return;
    // XDG_CURRENT_DESKTOP is only an environment variable. Run as root, or
    // set-uid, the exec would hand gsettings every capability back, and the
    // GIO and dconf modules it loads are chosen by that same environment.
    if (!mayAskGsettings(::getuid(), ::geteuid())) {
        log::info("[native] input: GNOME's natural scrolling is not read as root — the wheel "
                  "goes as the client sends it");
        return;
    }
    if (::access(kGsettings, X_OK) != 0) {
        log::info("[native] input: GNOME without /usr/bin/gsettings — its natural scrolling "
                  "setting cannot be read, the wheel goes as the client sends it");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stop = false;
        m_Wanted = true;
        m_LastAsk = Clock::now();
    }
    m_Thread = std::thread([this] { run(); });
}

void GnomeNaturalScroll::stop()
{
    if (!m_Thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stop = true;
    }
    m_Wake.notify_one();
    m_Thread.join();
    m_Reversed.store(false, std::memory_order_relaxed);
}

bool GnomeNaturalScroll::reversed()
{
    if (!m_Thread.joinable()) return false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto now = Clock::now();
        if (now - m_LastAsk >= kReadEvery) {
            m_LastAsk = now;
            m_Wanted = true;
            m_Wake.notify_one();
        }
    }
    return m_Reversed.load(std::memory_order_relaxed);
}

void GnomeNaturalScroll::run()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Wake.wait(lock, [this] { return m_Stop || m_Wanted; });
            if (m_Stop) return;
            m_Wanted = false;
        }
        readOnce();
    }
}

void GnomeNaturalScroll::readOnce()
{
    std::string out;
    const int code = askGsettings(out);
    const std::optional<bool> answer = code == 0 ? parseGsettingsBool(out) : std::nullopt;
    if (!answer) {
        // Keep the last answer: one slow or failed read is no reason to flip
        // the wheel. Said once, not at every turn of it.
        if (!m_FailLogged) {
            m_FailLogged = true;
            log::info("[native] input: gsettings gave no answer for GNOME's natural scrolling "
                      "(exit " +
                      std::to_string(code) + ") — the wheel keeps its last direction");
        }
        return;
    }
    m_Reversed.store(*answer, std::memory_order_relaxed);
    if (m_Logged != static_cast<int>(*answer)) {
        m_Logged = static_cast<int>(*answer);
        log::info(*answer ? "[native] input: GNOME natural scrolling is on for mice — the wheel is "
                            "sent reversed, so the page scrolls the way the client does"
                          : "[native] input: GNOME natural scrolling is off for mice — the wheel "
                            "is sent as the client scrolls");
    }
}

} // namespace mw::native::input
