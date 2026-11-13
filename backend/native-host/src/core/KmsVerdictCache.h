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

// Whether a card's scanout can be read, kept a few seconds per card. Pure, so
// that it is tested everywhere.
//
// The Linux probe answers that question by trying: it reads the buffer a plane
// is showing, through moonlightweb-kms when the process does not hold the
// capability itself (KmsCapture::canReadFramebuffers). The probe runs at every
// request of the status route, so a page that asks it several times a second
// would start a helper and read a picture of the screen each time. The answer
// does not change that fast: a screen turned off, a seat handed to another
// user, a package installed or removed. Three seconds old is fresh enough for
// a list of hosts.
//
// Not for a session: the capture it is about to choose (KMS, or the portal and
// its dialog) depends on the answer of now, a seat given back a second ago
// included. NativeHost::createSession clears the verdicts before its probe.
//
// The check runs under the lock, and the clock is read under it too: callers
// that arrive while a card is being asked wait for that answer and find it
// fresh, instead of each starting a helper of their own. The helper's answer
// is bounded by its own timeout.

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace mw::native {

/// How long a card's verdict is kept.
constexpr std::chrono::milliseconds kKmsVerdictTtl{3000};

class KmsVerdictCache
{
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    struct Verdict
    {
        bool readable = false;
        std::string why; ///< the reason, when it cannot be read
    };

    /// @p now is the clock, read under the lock at each get(); the tests give
    /// one of their own.
    explicit KmsVerdictCache(Clock::duration ttl = kKmsVerdictTtl, Now now = &Clock::now)
        : m_Ttl(ttl)
        , m_Now(std::move(now))
    {}

    KmsVerdictCache(const KmsVerdictCache&) = delete;
    KmsVerdictCache& operator=(const KmsVerdictCache&) = delete;

    /// The verdict for @p card: the one kept if it was asked less than the ttl
    /// ago, otherwise `check(why)` asked again and kept. A clock that reads
    /// before the kept time asks again.
    template <typename Check> Verdict get(const std::string& card, Check&& check)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        const Clock::time_point now = m_Now();
        auto it = m_Entries.find(card);
        if (it != m_Entries.end() && now >= it->second.at && now - it->second.at < m_Ttl)
            return it->second.verdict;
        Verdict verdict;
        verdict.readable = std::forward<Check>(check)(verdict.why);
        if (verdict.readable) verdict.why.clear();
        m_Entries[card] = Entry{verdict, now};
        return verdict;
    }

    /// Forget every verdict: the next get() of each card asks it again.
    void clear()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Entries.clear();
    }

private:
    struct Entry
    {
        Verdict verdict;
        Clock::time_point at;
    };

    std::mutex m_Lock;
    std::map<std::string, Entry> m_Entries;
    Clock::duration m_Ttl;
    Now m_Now;
};

/// The process's verdicts: the Linux probe's, cleared by a session's creation.
inline KmsVerdictCache& kmsVerdicts()
{
    static KmsVerdictCache verdicts;
    return verdicts;
}

} // namespace mw::native
