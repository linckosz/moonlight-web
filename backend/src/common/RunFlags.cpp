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

#include "RunFlags.h"
#include "Logger.h"

#include <QLoggingCategory>
#include <QString>

#include <rtc/global.hpp>

#include <atomic>
#include <mutex>

namespace mw::run {

namespace {
std::atomic<bool> g_DebugCli{false};
std::atomic<bool> g_DebugSetting{false};
std::atomic<bool> g_VerboseCli{false};

// libdatachannel's lines, into ours. Its errors are ours to see in any mode;
// everything below them is the chatter --verbose asked for.
void rtcLine(rtc::LogLevel level, std::string message)
{
    const QString line = QStringLiteral("[rtc] ") + QString::fromStdString(message);
    if (level <= rtc::LogLevel::Warning)
        Logger::warning(line);
    else
        Logger::debug(line);
}
} // namespace

void setDebugFromCli(bool on)
{
    g_DebugCli = on;
}

void setDebugSetting(bool on)
{
    g_DebugSetting = on;
}

bool debugFromCli()
{
    return g_DebugCli;
}

bool debug()
{
    return g_DebugCli || g_DebugSetting;
}

void setVerboseFromCli(bool on)
{
    g_VerboseCli = on;
}

bool verboseFromCli()
{
    return g_VerboseCli;
}

bool verbose()
{
    return g_VerboseCli || debug();
}

void applyVerboseLogging()
{
    static std::mutex mutex;
    static bool applied = false; // what the last call set up
    static bool rtcInstalled = false;
    std::lock_guard lock(mutex);
    const bool on = verbose();
    if (on == applied) return;
    applied = on;

    Logger::instance()->setMinLevel(on ? Logger::Debug : Logger::Info);
    // Qt's own diagnostics for what a remote connection goes through: TLS,
    // HTTP, sockets. Everything else of Qt's (qpa, input...) stays quiet.
    QLoggingCategory::setFilterRules(on ? QStringLiteral("qt.network*.debug=true\n"
                                                         "qt.websockets*.debug=true")
                                        : QString());
    // Never installed until asked for, so an ordinary run keeps the silence it
    // always had. Once installed, off is the None level: without a callback it
    // would print to stdout, which carries the worker's protocol.
    if (on || rtcInstalled) {
        rtc::InitLogger(on ? rtc::LogLevel::Debug : rtc::LogLevel::None, rtcLine);
        rtcInstalled = true;
    }
    Logger::info(on ? QStringLiteral("Verbose logging on") : QStringLiteral("Verbose logging off"));
}

} // namespace mw::run
