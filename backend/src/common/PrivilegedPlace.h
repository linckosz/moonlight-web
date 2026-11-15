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

#include <QFile>
#include <QString>
#include <QStringList>

/**
 * Where a process that runs with more rights than its user keeps its own
 * files: logs, crash dumps, the bench's traces.
 *
 * One rule, decided from the process's own token and never from its command
 * line, so every helper is covered, the ones to come included:
 *
 *   - A process with its user's own rights writes where it always has: the
 *     per-user AppDataLocation.
 *   - On Windows, a process that runs as SYSTEM (or another service account)
 *     or with an elevated token writes under the install directory, in
 *     `host-data`, a directory only SYSTEM and Administrators may write to:
 *       host-data\logs\<SID>   readable by that account, for its own logs
 *       host-data\crashes      SYSTEM and Administrators only
 *     Each directory is made here with that DACL, or checked to have it.
 *   - On Linux and macOS, a process whose effective uid is 0 (or differs from
 *     its real uid) writes in its AppDataLocation only when that directory and
 *     every one above it belong to root and nobody else may write to them.
 *
 * When no such place can be had, the process writes no file at all: it says
 * so once on stderr and keeps running. A stream worker's stderr reaches the
 * server's own log, so its lines are not lost.
 */
namespace mw::place {

/// Whether this process holds more rights than its user's own session.
/// Windows: the LocalSystem, LocalService or NetworkService account, or an
/// elevated token. POSIX: an effective uid of 0, or one other than the real
/// uid. Read from the process's own token, once.
bool isPrivileged();

/// The directory this process writes its logs to, made if missing.
/// @p readerSid: on Windows, the account given read access to a privileged
/// process's logs, as a SID string; empty means this token's own user. Not
/// used otherwise. Empty result: no place could be had (said on stderr).
QString logDir(const QString& readerSid = QString());

/// The directory this process writes its crash dumps to, made if missing.
/// Empty: no place could be had.
QString crashDir();

/// The directories the "Download logs" archive reads, newest file per kind
/// across them. A privileged process reads its own host-data\logs\<SID>
/// only. Any other reads its AppDataLocation logs, plus its account's
/// host-data\logs\<SID> when that directory exists and checks out.
QStringList readableLogDirs();

/// Open @p file, whose name is set, for appending in text mode, as the log
/// does. In a privileged process the file must be a regular file with a
/// single link, in a directory that passes the check above; anything else is
/// refused and @p why says what. Any other process opens it as QFile would.
bool openForAppend(QFile& file, QString* why = nullptr);

/// Whether @p dir is a directory this process may write its own files to by
/// path: always true for an unprivileged process, the check above otherwise
/// (on Windows, @p dir and every directory up to the install directory).
bool isWritablePlace(const QString& dir);

/// Where a bench trace (a click trace, the relay's frame and audio logs) is
/// written: beside @p logFilePath, the log this process writes. With no log,
/// the temp directory for an unprivileged process, and nowhere (empty) for a
/// privileged one.
QString traceDir(const QString& logFilePath);

/// At the start of a privileged stream worker: its own files in @p dir (its
/// logs and the bench's traces, named after each run) older than
/// @p maxAgeDays go. Nothing else in the directory is touched.
void pruneWorkerFiles(const QString& dir, int maxAgeDays = 7);

#ifdef Q_OS_WIN
/// Delete @p path without ever following a reparse point: a junction or a
/// symbolic link is removed as itself, never walked into.
bool removeNoFollow(const QString& path);
#endif

/// The uninstaller's verb (--remove-host-data): the crash dumps always, the
/// logs too with @p withLogs. Returns the process exit code: 0 when done.
int removeHostData(bool withLogs);

/// The SID string of this process's own user, empty off Windows or when the
/// token cannot be read.
QString currentUserSid();

// ── For the TNR ─────────────────────────────────────────────────────────────

/// Act as a process whose install directory is @p installDir, privileged or
/// not, with @p trustedSids standing in for Administrators and SYSTEM (they own
/// and may write the directories made here). On POSIX the first is the uid
/// that stands in for root, and @p installDir stands in for the data
/// directory: the chain is checked from it down. An unelevated test can then
/// make and check the same directories a privileged process would.
void setForTests(const QString& installDir, const QStringList& trustedSids, bool privileged = true);
/// Back to the process's real token.
void resetForTests();

#ifdef Q_OS_WIN
namespace win {

enum class Check
{
    Ok,
    Missing,
    ReparsePoint, ///< a junction or a link, or a path that resolves elsewhere
    NotDirectory,
    Owner, ///< owned by someone other than the trusted accounts
    Acl,   ///< a DACL that is not protected, or lets someone else write
    Error, ///< could not be read
};
QString describe(Check check);

/// Administrators, then SYSTEM: who owns and may write the directories made
/// here, outside the TNR.
QStringList defaultTrustedSids();

enum class Dir
{
    HostData, ///< host-data, and host-data\logs: listed by any signed-in account
    UserLogs, ///< host-data\logs\<SID>: read by that account
    Crashes,  ///< host-data\crashes: nobody else
};
/// The security descriptor each directory is made with, as SDDL: owned by the
/// first of @p trustedSids, full control for each of them, protected from
/// inheritance. @p readerSid is the account of a UserLogs directory.
QString lockedSddl(Dir dir, const QStringList& trustedSids, const QString& readerSid = QString());

/// A directory made here: protected DACL, owned by one of @p trustedSids, and
/// no ACE letting another account write, delete, add or re-ACL anything.
Check checkLockedDir(const QString& path, const QStringList& trustedSids);

/// The install directory: owned by SYSTEM, Administrators or TrustedInstaller
/// (or one of @p trustedSids), and no ACE on the directory itself that lets
/// another account rename, delete, re-ACL it or add to it. @p finalPath gets
/// the directory's path as the file system resolves it, the one every path
/// below it is built from.
Check checkInstallDir(const QString& path, const QStringList& trustedSids,
                      QString* finalPath = nullptr);

} // namespace win
#endif

} // namespace mw::place
