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
#include <QList>
#include <QPair>
#include <QString>

#include <cstdint>
#include <memory>

/// The record of what this host wrote in the router's table.
///
/// The entries are removed when the host stops. One that is killed leaves
/// them: a crash, a power cut, an installer that ends the process. They then
/// last one lease, or for as long as a router that keeps no lease runs. The
/// next start takes them back (RouterPortCore::sweep), and may only take back
/// what is provably this installation's:
///
///   by the record   each write is noted here before the router is asked, so
///                   only numbers this installation wrote are ever looked at.
///                   The router's whole table is never read.
///   by the text     an entry's description ends with a tag of this
///                   installation, six characters drawn once. Another
///                   MoonlightWeb on the LAN, or another edition on this
///                   machine, writes the same words and another tag.
///
/// The settings keep numbers to ask for again (router_ports); they are a
/// preference, overwritten when a number moves, and say nothing of what is on
/// the router.
namespace mw::routerholes {

/// What an entry is for: its description, and the numbers it may have.
enum class Kind
{
    Tunnel,
    /// One of the ports every stream shares.
    Streams,
    /// A stream slot's own port.
    Media,
};

/// One entry of the router's table, as this host wrote it.
struct Hole
{
    uint16_t external = 0;
    bool tcp = false;
    uint16_t internal = 0;
    Kind kind = Kind::Tunnel;
    /// The stream slot of a Media entry, -1 otherwise.
    int slot = -1;
    /// The address this host had on the router's LAN when it wrote it.
    QString lan;
    /// Written by a version that kept no record and put no tag: known from
    /// the numbers the settings remember, and by the product's words alone.
    bool old = false;

    bool operator==(const Hole& o) const
    {
        return external == o.external && tcp == o.tcp && internal == o.internal && kind == o.kind &&
               slot == o.slot && lan == o.lan && old == o.old;
    }
};

/// What the router's page shows for an entry: the product's words, then the
/// installation's tag when there is one.
QString describe(Kind kind, int slot, const QString& tag);

/// Six characters, 0-9 and a-f.
bool validTag(const QString& tag);

/// An IPv4 address in figures, written the one way a router writes it: four
/// numbers of 0 to 255, no sign, no space, no leading zero.
bool dottedIpv4(const QString& text);

/// Whether this kind of instance may have written that line: its numbers are
/// within the lists its kind draws from (RouterPortPools.h), and fit what the
/// line says it is for. A line that is not is never acted on, so a record
/// altered by hand leads nowhere outside this host's own numbers.
bool allowed(bool devInstance, const Hole& hole);

/// A port no MoonlightWeb ever asks a router for: between the installed
/// host's tunnel pool and a DEV identity's. The router is asked about it
/// before anything is removed, and must answer that it has no such entry.
constexpr uint16_t kKnownQuestionPort = 46063;

/// What RouterPortCore needs of the record.
class IRecord
{
public:
    virtual ~IRecord() = default;

    /// This installation's tag: never empty.
    virtual QString tag() const = 0;
    virtual QList<Hole> holes() const = 0;
    /// The line of that port and protocol, when there is one.
    virtual bool find(uint16_t external, bool tcp, Hole& out) const = 0;
    /// Before the router is written to: the line of that port and protocol
    /// is this one from now on.
    virtual void note(const Hole& hole) = 0;
    /// The router no longer has the entry, or it is no longer this host's.
    virtual void strike(uint16_t external, bool tcp) = 0;
};

/// What a version that kept no record remembers in the settings.
struct Earlier
{
    /// router_ports.lan_ip: empty when no such version ever found a router.
    QString lan;
    QList<quint16> tunnel;
    /// (stream slot, router-side port)
    QList<QPair<int, quint16>> media;
};

/// The lines those numbers stand for, as such a version wrote them: a tunnel
/// port forwarded to itself, a slot's port forwarded to 48550 + slot, in UDP
/// and in TCP. None unless the numbers were obtained under `lanNow`.
QList<Hole> earlierHoles(bool devInstance, const Earlier& earlier, const QString& lanNow);

// ── The file ────────────────────────────────────────────────────────────────

struct Content
{
    /// Whose record it is: fingerprint().
    QString owner;
    QString tag;
    QList<Hole> holes;
};

/// The most a record file may weigh, and the most lines it may hold: every
/// number every list has, in both protocols, is well under it.
constexpr qsizetype kFileMax = 64 * 1024;
constexpr qsizetype kHolesMax = 512;

QByteArray serialize(const Content& content);

/// Strict: false for anything that is not a record as serialize() writes it.
/// A line that is malformed, or that this kind of instance may not have
/// written, is left out and the rest is kept.
bool parse(const QByteArray& json, bool devInstance, Content& out);

/// Which installation a record belongs to: this machine and this data
/// directory. A profile copied to another directory, or restored on another
/// machine, brings a record that is not this installation's.
QString fingerprint(const QByteArray& machineId, const QString& directory);

/// The record on disk: router_holes.json in the data directory, beside a lock
/// the system holds for as long as this process lives.
///
/// One process at a time: while its process runs the record lists LIVE
/// entries, and a second process that took them for leftovers would remove
/// them. The lock is the system's own, on an open file, so it ends with the
/// process however it ends.
///
/// Used from one thread, the allocator's.
class File : public IRecord
{
public:
    /// Null when another process holds the record (`said` says so): this run
    /// then keeps none. A record that is not this installation's, or does not
    /// read as one, is replaced by an empty one with a tag of its own. Where
    /// there was no file at all, the lines of `earlier` are the first ones.
    static std::unique_ptr<File> open(const QString& directory, bool devInstance,
                                      const QByteArray& machineId, const Earlier& earlier,
                                      const QString& lanNow, QString* said);
    ~File() override;

    QString tag() const override { return m_Content.tag; }
    QList<Hole> holes() const override { return m_Content.holes; }
    bool find(uint16_t external, bool tcp, Hole& out) const override;
    void note(const Hole& hole) override;
    void strike(uint16_t external, bool tcp) override;

private:
    File() = default;
    void save();

    QString m_Path;
    Content m_Content;
    /// The lock file, open and locked: a HANDLE on Windows, a descriptor
    /// elsewhere.
    intptr_t m_Lock = -1;
    bool m_SaveFailed = false;
};

} // namespace mw::routerholes
