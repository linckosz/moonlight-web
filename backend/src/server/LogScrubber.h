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

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

/**
 * What the "Download logs" archive does to every file before it goes in: the
 * archive is made to be attached to a public GitHub issue, and a log written
 * for its owner holds what nobody else should read.
 *
 * Hidden, as "(hidden)": what grants access or decrypts — PINs, passwords,
 * tokens, keys (rikey, pairing secrets, ICE credentials, the tunnel's host
 * key), cookies and Authorization headers, PEM blocks, the client's unique id,
 * the rendezvous and share links, and what a person typed or looked at
 * (keyboard diagnostics, window titles, geolocation, e-mail and MAC addresses).
 *
 * Replaced, the same value by the same stand-in across the whole archive so a
 * reader can still follow one machine or one address from line to line, each
 * in braces — a label, not part of the address:
 *   - machines by capital letters: {A} is always this PC, the hosts and the
 *     other machines named in the log {B}, {C}... in order of appearance. A
 *     machine's names (Names, and LAN names such as "Kids-iPad.local") and
 *     its LAN addresses get the same letter: "{B} is online (192.168.x.{B})".
 *     A second address of one machine under the same prefix is {A2};
 *   - LAN addresses of no known machine (private ranges, CGNAT/Tailscale,
 *     link-local, ULA) by a small letter for their /24 (/64 in IPv6) and a
 *     number for the address in it: 192.168.x.{a1}, 10.x.x.{b2}, 100.x.x.{c1},
 *     fd::{d1}, fe80::{e1}; .0 and .255 keep their number ({a0}, {a255});
 *   - public addresses by a number under a prefix no address has:
 *     256.x.x.{1}, 2xxx::{1}. Never a machine's letter: machines behind one
 *     router share its public address;
 *   - the instance's name and the OS account by instance-name and user, the
 *     account in home paths (C:\Users\<name>, /home/<name>, /Users/<name>)
 *     too, and the instance's own moonlightweb.top subdomain by instance.
 * Loopback, multicast and 0.0.0.0 stay: they are the same on every machine.
 *
 * A best effort over free text, not a guarantee: it knows the formats
 * MoonlightWeb writes, and the names it is told.
 */
class LogScrubber
{
public:
    struct Machine
    {
        QStringList names;     ///< what it is called (its name, an alias)
        QStringList addresses; ///< what it is reached at
    };
    struct Names
    {
        Machine thisMachine;  ///< {A}
        QList<Machine> hosts; ///< the streaming hosts
        QString instance;     ///< the instance name the owner chose (instance-name)
        QString user;         ///< the OS account the server runs as (user)
        /// The titles of the hosts' apps, each replaced by "app": what someone
        /// plays is theirs. Empty for the "Download logs" archive, which keeps
        /// them; filled for the error reports (ErrorReports.h), which leave this
        /// PC without anyone reading them first.
        QStringList apps;
        /// The names of the paired devices (AuthManager sessions), each
        /// replaced by "device", and of the guests the owner named on the
        /// share board, by "guest": both often carry a first name. Like apps,
        /// for the error reports only.
        QStringList devices;
        QStringList guests;
        /// The cities the paired devices were seen from (AuthManager), each
        /// replaced by "city": one comes from where a person is. Error reports
        /// only, like the three above.
        QStringList cities;
    };

    LogScrubber();
    explicit LogScrubber(const Names& names);

    /// A whole file (UTF-8), line by line.
    QByteArray scrub(const QByteArray& text);
    /// One line, no newline.
    QString scrubLine(QString line);

    /// For about.txt: what was done to the files, in a few lines.
    static QString notice();

private:
    /// A machine's tag, {A}; its letters are given on first use, and this
    /// PC's (machine 0) are A from the start.
    QString machineTag(int machine);
    /// The machine a name belongs to: a known one, or one met in the log.
    int machineNamed(const QString& name);
    /// A known machine's address: its letters, numbered from the second
    /// address under the same @p prefix ({A}, {A2}).
    QString machineAddress(int machine, const QString& prefix, const QString& address);
    /// An address of no known machine: small letters for its @p subnet, then
    /// its number in order of appearance, or @p fixed if >= 0 ({a1}).
    QString unknownLan(const QString& subnet, const QString& host, int fixed);
    /// A public address: its number in order of appearance ({1}).
    QString publicAddress(const QString& key);
    void replaceNames(QString& line);
    void replaceAddresses(QString& line);

    struct NameRule
    {
        QString name;
        int machine = -1;         ///< -1 for the instance's name and the account
        QString word;             ///< their stand-in
        QRegularExpression match; ///< the name as a whole word, any case
    };
    // Longest first, so "leos-macbook-pro2.home" goes before "leos-macbook-pro2".
    QList<NameRule> m_Names;
    QHash<QString, int> m_NameMachine;             ///< lower-cased name → machine
    QHash<QString, int> m_AddressMachine;          ///< address → machine
    int m_Machines = 1;                            ///< known and met so far (this PC is 0)
    QHash<int, QString> m_MachineLetters;          ///< machine → its letters
    int m_Lettered = 1;                            ///< letters given (A to this PC)
    QHash<QString, QStringList> m_PrefixAddresses; ///< "machine|prefix" → its addresses
    QHash<QString, QString> m_Stand;               ///< value → its stand-in
    QHash<QString, QString> m_SubnetLetters;       ///< subnet → its letters
    QHash<QString, int> m_SubnetHosts;             ///< subnet → addresses numbered so far
    int m_Subnets = 0;
    int m_Public = 0;
};
