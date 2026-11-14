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

#include <QString>

#include <functional>

/**
 * `MoonlightWeb.exe --quit-server`: what the installer runs before an
 * uninstall or an update kills the server. It asks the running server of
 * this install to quit over the local channel (LocalChannel.h), waits for
 * that process to end, then says whether the record of what this host wrote
 * on the router (router_holes.json) still holds lines: what is left on the
 * box. Windows only; the installer is its one caller.
 *
 * It runs elevated, in a folder the user owns, before anything else the
 * program does on start: it creates nothing, writes nothing, takes no lock,
 * loads no settings. The record is read, never through routerholes::File
 * (which takes a lock and may put a file in place), with every folder on the
 * way checked not to lead elsewhere, and the file's own final path compared
 * with the one asked for.
 */
namespace mw::quitserver {

/// What the verb ends with: its exit code.
enum class Code : int
{
    /// The server was asked, and is gone; the record holds no line.
    Gone = 0,
    /// The server was asked, and is gone; the record still holds lines.
    LinesLeft = 1,
    /// Nobody answered (no server, another account's, another install's);
    /// the record holds no line.
    NobodyToAsk = 2,
    /// The server was asked, and was still running at the limit. The
    /// record is not read.
    StillRunning = 3,
    /// The server answered the request with a refusal.
    Refused = 4,
    /// The record could not be read as it must be.
    RecordUnreadable = 5,
    /// Nobody answered, and the record holds lines.
    NobodyLinesLeft = 6,
    /// The server was asked, and is gone; it ran under another account (a
    /// service's, reached by the people signed in), whose record is in that
    /// account's profile: not read, so nothing is known of what it left.
    GoneOtherAccount = 7,
};

/// Whether the process behind @p process (a HANDLE opened with
/// PROCESS_QUERY_LIMITED_INFORMATION) runs under this process's own account,
/// by the user SID of both tokens. False when either cannot be read.
bool sameAccount(void* process);

/// How long the verb waits for each thing. The product's are these; tests
/// pass shorter ones.
struct Limits
{
    /// To open the channel, ask, and be answered.
    int channelMs = 3000;
    /// For the server's process to end, once it said yes.
    int exitMs = 12000;
    /// The verb's own process ends at this, whatever it is doing.
    int watchdogMs = 17000;
};

/// What reading the record found.
struct Count
{
    /// False: the record could not be read as it must be (`why`).
    bool read = false;
    /// Its lines, this kind of instance's and the other's.
    int lines = 0;
    QString why;
};

/// The lines of `<roaming>\MoonlightWeb\<dataName>\router_holes.json`. No
/// file (or no folder) is no line. Refused when the roaming folder's parent,
/// the roaming folder or one of the two below it is a reparse point or not a
/// folder, when the file is a reparse point or not a plain file, when it is
/// larger than a record may be, when its final path is not the one asked for
/// (a folder swapped for a link after it was checked), or when it does not
/// parse as a record. Opens nothing for writing. @p betweenChecks runs
/// after the folders are checked and before the file is opened: tests only.
Count countRecordLines(const QString& roaming, const QString& dataName,
                       const std::function<void()>& betweenChecks = {});

/// This account's roaming application data folder (FOLDERID_RoamingAppData).
/// The user can point it elsewhere (HKCU User Shell Folders): acceptable only
/// because the verb reads there and never writes. It must stay so.
QString roamingAppData();

/// The verb, with what it talks to and where it reads given (tests). @p said
/// gets one line on what happened, with no address in it. @p sameAs decides
/// whether the server that answered is this account's (sameAccount() when
/// empty; tests stand in for it).
Code run(const QString& endpoint, const QString& roaming, const QString& dataName,
         const Limits& limits, QString* said = nullptr,
         const std::function<bool(void*)>& sameAs = {});

/// Whether this start is the verb: `--quit-server`, and nothing else.
bool isTheVerb(int argc, char** argv);

/// The verb as the installer runs it: this install's channel, this account's
/// record, the product's limits, a watchdog; one line on standard error.
int runVerb();

} // namespace mw::quitserver
