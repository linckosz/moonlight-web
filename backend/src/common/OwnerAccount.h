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

/// The account this installation was made by: the owner, the one the server
/// streams for.
///
/// A server that runs as a service puts its stream worker in the console
/// session, as whoever sits there. Most of the time that is the owner. It can
/// also be another account of the machine, and what the server would give a
/// process of its owner it must not give a process of that one. So the
/// installer, which runs elevated as the installing account, has this
/// executable record that account where only an administrator or SYSTEM can
/// write: HKLM\SOFTWARE\<product>\Setup, OwnerSid. A place an ordinary account
/// could write would be that account's to forge.
///
/// A SID, never a name. With nothing recorded (an installation older than the
/// record, a build tree) nobody is the owner and the answer is no, until the
/// next install or repair records one. No guess is made from who logs on.
///
/// An installer the system itself runs is nobody's: a server that is a
/// service updates itself that way, and a deployment tool installs that way.
/// Such a run records nothing and leaves what an earlier install recorded.
///
/// Windows only: elsewhere nothing is recorded and nobody is the owner.
namespace mw::owneraccount {

/// Where a record is kept: a registry key, in the machine's hive or in the
/// current user's.
///
/// The product has one, machineStore(), and passes it at every call: there is
/// no switch that moves it. The current user's hive is for the tests, which
/// may not write the machine's; a record kept there would be the console
/// user's to write, and nothing that decides anything may read it.
struct Store
{
    enum class Hive
    {
        Machine,
        CurrentUser,
    };
    Hive hive = Hive::Machine;
    QString key;
};

/// The installer's place, and the only one the product reads:
/// HKLM\SOFTWARE\<product>\Setup.
Store machineStore();

/// Installer, elevated: records the account this process runs as. False, with
/// the reason, when it cannot be read or written, and when it is one of the
/// system's own (isServiceAccount): the record is then left as it was.
bool record(const Store& store, QString* why);

/// The record goes (the uninstaller removes the whole key itself).
void forget(const Store& store);

/// The recorded account, as a SID in text ("S-1-5-21-…"). Empty when none is
/// recorded or it cannot be read.
QString recorded(const Store& store);

/// The account behind a Windows access token (a HANDLE opened with
/// TOKEN_QUERY), as a SID in text. Empty when it cannot be read.
QString accountOf(void* token);

/// The account a running process is, by its id, as a SID in text: its token,
/// read with no more than PROCESS_QUERY_LIMITED_INFORMATION and TOKEN_QUERY.
/// Empty when the process is gone, cannot be opened, or is not on Windows.
QString accountOfProcess(quint32 pid);

/// Whether two accounts are one: both given, both well-formed SIDs, and
/// equal. Anything missing or malformed is « no ».
bool same(const QString& recordedSid, const QString& accountSid);

/// Whether an account is one of the system's own (LocalSystem, LocalService,
/// NetworkService): what a service runs as, and nobody's account. It is
/// never recorded and never the owner.
bool isServiceAccount(const QString& sid);

/// Whether `token` is the account recorded in `store`. Never one of the
/// system's own, whatever is recorded.
bool isOwner(const Store& store, void* token);

/// What the installer is told when it asks, as the person who started it,
/// whether the server may be started as that account (`--has-user-rights`).
/// The values are the verb's exit codes: the installer starts the server on
/// Yes and on OtherAccount, and follows its first steps on Yes alone (another
/// account's server keeps its files where the installer does not look).
enum class StartAs
{
    Yes = 0,          ///< its user's own rights, and the account the install was made by
    MoreRights = 1,   ///< more rights than its user: not started this way
    OtherAccount = 8, ///< its user's own rights, another account than the recorded one
};
StartAs startAs(bool privileged, const QString& recordedSid, const QString& accountSid);

/// What the installer is told when it asks, as itself, which account it runs
/// as (`--setup-account`): the system's own, or a person's. The values are
/// the verb's exit codes. Person comes from one path only, a SID that reads
/// and is not LocalSystem's: an account that cannot be read is Unknown, which
/// the installer takes for the system, and so is any code it does not know.
enum class SetupAccount
{
    Person = 0,  ///< a person's account, read from the token
    Unknown = 2, ///< no SID, or one that does not read
    System = 18, ///< LocalSystem (S-1-5-18)
};
SetupAccount setupAccount(const QString& sid);

} // namespace mw::owneraccount
