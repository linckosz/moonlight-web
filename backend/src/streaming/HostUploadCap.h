/*
 * MoonlightWeb — self-hosted web client for Moonlight game streaming.
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
#include <string>

// `hostcap=<Mbit/s>`, a bench key, off unless asked: during a native stream,
// every other upload of the host PC is capped, so that a queue it would fill
// further down the path stays empty and the stream goes first.
//
// ── Why ─────────────────────────────────────────────────────────────────────
//
// 24/10/2026: the stream from DualRTX to a Wi-Fi client froze for 2 to 27 s
// whenever TeraBox uploaded 40-125 Mbit/s from DualRTX itself. The stream
// already left marked AF41. The queue sits in the radio of the Freebox
// repeater the host hangs off, on its way up to the box (measured 25/10). The
// one lever left to the host, short of touching the house's network, is its
// own upload: kept under what the path carries, the queue there stays empty.
//
// ── How ─────────────────────────────────────────────────────────────────────
//
// Two Windows QoS policies, in the ActiveStore only (gone at the next reboot,
// never in the registry), named after the worker's process id:
//  - "MoonlightWeb stream <pid>": this executable's own traffic, its DSCP
//    (PacketMarking's video value; one policy holds one value, so the sound's
//    EF is not expected to survive this key, not measured), no cap, precedence 255;
//  - "MoonlightWeb host cap <pid>": all TCP and UDP, capped at the rate.
// Microsoft's precedence rules ("Manage QoS Policy") say a policy that names
// an application's path beats one that names none, so the stream would keep
// its rate. Measured on 25/10/2026, it does not: at the same precedence (127,
// the default) the cap wins, the stream was capped with everything else, by
// its path, by its name alone, and against a -Default cap too. With
// -Precedence 255 on the application's policy, a stand-in executable kept 3 ms
// while every other flow queued 250-370 ms behind the cap (S1 bis). A bench
// key until a real stream has shown it too. The QoS Packet Scheduler applies
// the cap on the adapters it is bound to (vEthernet (LAN) on DualRTX); a
// Hyper-V VM leaves by the physical adapter and is not capped.
//
// Setting a policy takes an administrator: the native host's worker, which
// the launcher service starts as SYSTEM, has the rights; the server, and a
// worker on the user's plain token, do not, and say so in the log. The
// policies are set by PowerShell's NetQos cmdlets, run with -EncodedCommand
// from a script built here (the executable's path is the only outside text in
// it, quoted). Removal is a detached process, so it finishes after the worker
// has gone; it runs twice, 5 s apart, in case the setting is still under way.
// A worker that dies before it removes its own leaves them until the next
// worker starts, which sweeps the policies of process ids no longer running,
// or until a reboot.
//
// Windows only: Linux would need tc and CAP_NET_ADMIN, macOS dummynet and
// root (plan « le stream passe avant un gros envoi du PC hôte », S4).
namespace mw::hostcap {

/// Above that, a cap is no cap: refused by the key.
constexpr unsigned kMaxMbps = 10000;

std::string capName(unsigned long pid);
std::string streamName(unsigned long pid);

/// @p s as a PowerShell single-quoted literal.
std::string psQuote(const std::string& s);

/// Sweeps the policies of workers no longer running, then sets this worker's
/// two: the stream's first, so it is never capped in between.
std::string onScript(unsigned long pid, const std::string& exePath, unsigned mbps, unsigned dscp);

/// Removes this worker's two policies, the cap first; twice, 5 s apart.
std::string offScript(unsigned long pid);

/// @p script for powershell -EncodedCommand: UTF-16LE, base64.
std::string encodedCommand(const std::string& script);

/// One worker's cap: set once by start(), removed by stop() or the destructor.
/// Thread-safe; start() returns at once.
class Cap
{
public:
    Cap() = default;
    Cap(const Cap&) = delete;
    Cap& operator=(const Cap&) = delete;
    ~Cap() { stop(); }

    /// Cap every other upload at @p mbps, the stream's own marked @p dscp.
    /// Nothing when @p mbps is 0, when already started, without the rights
    /// (logged), or off Windows (logged).
    void start(unsigned mbps, unsigned dscp);
    void stop();

private:
    std::mutex m_Mutex;
    bool m_Started = false;
};

} // namespace mw::hostcap
