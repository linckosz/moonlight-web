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

#include "RouterHoles.h"
#include "RouterPortCore.h"
#include "RouterPortPools.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStringList>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mw::routerholes {

namespace {

const QString kUdp = QStringLiteral("UDP");
const QString kTcp = QStringLiteral("TCP");

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::Tunnel: return QStringLiteral("tunnel");
    case Kind::Streams: return QStringLiteral("streams");
    case Kind::Tcp443: return QStringLiteral("tcp443");
    case Kind::Media: break;
    }
    return QStringLiteral("media");
}

bool kindOf(const QString& name, Kind& out)
{
    for (const Kind kind : {Kind::Tunnel, Kind::Streams, Kind::Media, Kind::Tcp443}) {
        if (kindName(kind) != name) continue;
        out = kind;
        return true;
    }
    return false;
}

bool lowerHex(const QString& text, qsizetype length)
{
    if (text.size() != length) return false;
    for (const QChar c : text) {
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool letter = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        if (!digit && !letter) return false;
    }
    return true;
}

/// A JSON number that is a whole one within [low, high].
bool wholeNumber(const QJsonValue& value, int low, int high, int& out)
{
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (!(d >= low && d <= high)) return false;
    const int n = static_cast<int>(d);
    if (static_cast<double>(n) != d) return false;
    out = n;
    return true;
}

bool has(const QList<Hole>& holes, uint16_t external, bool tcp)
{
    for (const Hole& line : holes)
        if (line.external == external && line.tcp == tcp) return true;
    return false;
}

/// One line of the file, every key it may have and no other.
bool lineOf(const QJsonValue& value, Hole& out)
{
    if (!value.isObject()) return false;
    const QJsonObject o = value.toObject();
    int port = 0;
    int to = 0;
    if (!wholeNumber(o.value(QStringLiteral("port")), 1, 65535, port) ||
        !wholeNumber(o.value(QStringLiteral("to")), 1, 65535, to))
        return false;
    const QJsonValue proto = o.value(QStringLiteral("proto"));
    const QJsonValue what = o.value(QStringLiteral("what"));
    const QJsonValue lan = o.value(QStringLiteral("lan"));
    if (!proto.isString() || !what.isString() || !lan.isString()) return false;
    if (proto.toString() != kUdp && proto.toString() != kTcp) return false;
    Hole line;
    if (!kindOf(what.toString(), line.kind) || !dottedIpv4(lan.toString())) return false;
    qsizetype keys = 5;
    if (line.kind == Kind::Media) {
        if (!wholeNumber(o.value(QStringLiteral("slot")), 0, 255, line.slot)) return false;
        ++keys;
    }
    if (o.contains(QStringLiteral("old"))) {
        if (!o.value(QStringLiteral("old")).isBool()) return false;
        line.old = o.value(QStringLiteral("old")).toBool();
        ++keys;
    }
    if (o.size() != keys) return false;
    line.external = static_cast<uint16_t>(port);
    line.internal = static_cast<uint16_t>(to);
    line.tcp = proto.toString() == kTcp;
    line.lan = lan.toString();
    out = line;
    return true;
}

QString newTag()
{
    return QString::number(QRandomGenerator::system()->bounded(1 << 24), 16)
        .rightJustified(6, QLatin1Char('0'));
}

/// The lock file, opened and locked by the system for this process alone:
/// -1 when another process has it, or when it cannot be opened.
intptr_t lockFile(const QString& path)
{
#ifdef _WIN32
    // Not inheritable: a child must not keep the record locked after us.
    const HANDLE h =
        CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()),
                    GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED firstByte{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &firstByte)) {
        CloseHandle(h);
        return -1;
    }
    return reinterpret_cast<intptr_t>(h);
#else
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
#endif
}

void unlockFile(intptr_t lock)
{
    if (lock == -1) return;
#ifdef _WIN32
    // Closing the handle is what gives the lock back
    CloseHandle(reinterpret_cast<HANDLE>(lock));
#else
    ::close(static_cast<int>(lock));
#endif
}

} // namespace

QString describe(Kind kind, int slot, const QString& tag)
{
    QString text;
    switch (kind) {
    case Kind::Tunnel: text = QStringLiteral("MoonlightWeb tunnel"); break;
    case Kind::Streams: text = QStringLiteral("MoonlightWeb streams"); break;
    case Kind::Media: text = QStringLiteral("MoonlightWeb media slot %1").arg(slot); break;
    case Kind::Tcp443: text = QStringLiteral("MoonlightWeb TCP 443"); break;
    }
    return tag.isEmpty() ? text : text + QLatin1Char(' ') + tag;
}

bool validTag(const QString& tag)
{
    return lowerHex(tag, 6);
}

bool dottedIpv4(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    for (const QString& part : parts) {
        if (part.isEmpty() || part.size() > 3) return false;
        if (part.size() > 1 && part.at(0) == QLatin1Char('0')) return false;
        int value = 0;
        for (const QChar c : part) {
            if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
            value = value * 10 + (c.unicode() - '0');
        }
        if (value > 255) return false;
    }
    return true;
}

bool allowed(bool devInstance, const Hole& hole)
{
    if (hole.external == 0 || hole.internal == 0) return false;
    switch (hole.kind) {
    case Kind::Tunnel:
        return hole.slot == -1 && hole.internal == hole.external &&
               RouterPortCore::tunnelPortAllowed(devInstance, hole.external);
    case Kind::Streams:
        return hole.slot == -1 && hole.internal == hole.external &&
               (RouterPortCore::muxPortAllowed(devInstance, RouterPortCore::kMuxStandard,
                                               hole.external) ||
                RouterPortCore::muxPortAllowed(devInstance, RouterPortCore::kMuxBlock,
                                               hole.external));
    case Kind::Media:
        return hole.slot >= 0 && hole.slot < mw::routerports::kMediaPortCount &&
               mw::routerports::inMediaBlock(hole.internal) &&
               RouterPortCore::mediaPortAllowed(hole.external);
    case Kind::Tcp443:
        // That one number, in TCP, towards an installed host's block port
        return !devInstance && hole.tcp && hole.external == 443 && hole.slot == -1 &&
               RouterPortCore::muxPortAllowed(false, RouterPortCore::kMuxBlock, hole.internal);
    }
    return false;
}

QList<Hole> earlierHoles(bool devInstance, const Earlier& earlier, const QString& lanNow)
{
    QList<Hole> out;
    if (earlier.lan.isEmpty() || earlier.lan != lanNow) return out;
    const auto both = [&](Hole line) {
        line.lan = earlier.lan;
        line.old = true;
        for (const bool tcp : {false, true}) {
            line.tcp = tcp;
            if (allowed(devInstance, line) && !has(out, line.external, tcp)) out.append(line);
        }
    };
    for (const quint16 port : earlier.tunnel) {
        Hole line;
        line.external = port;
        line.internal = port;
        line.kind = Kind::Tunnel;
        both(line);
    }
    for (const auto& [slot, port] : earlier.media) {
        if (slot < 0 || slot >= mw::routerports::kMediaPortCount) continue;
        Hole line;
        line.external = port;
        line.internal = static_cast<uint16_t>(mw::routerports::kMediaBasePort + slot);
        line.kind = Kind::Media;
        line.slot = slot;
        both(line);
    }
    return out;
}

QByteArray serialize(const Content& content)
{
    // One list in the file: which kind may have written a line is read from
    // its numbers, each time, and never taken from the file's word.
    QJsonArray holes;
    for (const QList<Hole>* lines : {&content.holes, &content.others}) {
        for (const Hole& line : *lines) {
            QJsonObject o{{QStringLiteral("port"), static_cast<int>(line.external)},
                          {QStringLiteral("proto"), line.tcp ? kTcp : kUdp},
                          {QStringLiteral("to"), static_cast<int>(line.internal)},
                          {QStringLiteral("what"), kindName(line.kind)},
                          {QStringLiteral("lan"), line.lan}};
            if (line.kind == Kind::Media) o[QStringLiteral("slot")] = line.slot;
            if (line.old) o[QStringLiteral("old")] = true;
            holes.append(o);
        }
    }
    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("owner"), content.owner},
                           {QStringLiteral("tag"), content.tag},
                           {QStringLiteral("holes"), holes}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool parse(const QByteArray& json, bool devInstance, Content& out)
{
    out = Content{};
    if (json.size() > kFileMax) return false;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject root = doc.object();
    int version = 0;
    if (root.size() != 4 || !wholeNumber(root.value(QStringLiteral("version")), 1, 1, version))
        return false;
    const QJsonValue owner = root.value(QStringLiteral("owner"));
    const QJsonValue tag = root.value(QStringLiteral("tag"));
    const QJsonValue holes = root.value(QStringLiteral("holes"));
    if (!owner.isString() || !tag.isString() || !holes.isArray()) return false;
    if (!lowerHex(owner.toString(), 32) || !validTag(tag.toString())) return false;
    const QJsonArray lines = holes.toArray();
    if (lines.size() > kHolesMax) return false;

    Content content;
    content.owner = owner.toString();
    content.tag = tag.toString();
    QList<Hole> elsewhere;
    for (const QJsonValue& value : lines) {
        Hole line;
        if (!lineOf(value, line)) continue;
        if (allowed(devInstance, line)) {
            if (!has(content.holes, line.external, line.tcp)) content.holes.append(line);
        } else if (allowed(!devInstance, line)) {
            elsewhere.append(line);
        }
    }
    // After this kind's own, whatever their order in the file
    for (const Hole& line : elsewhere) {
        if (has(content.holes, line.external, line.tcp) ||
            has(content.others, line.external, line.tcp))
            continue;
        content.others.append(line);
    }
    out = content;
    return true;
}

QString fingerprint(const QByteArray& machineId, const QString& directory)
{
    QString path = QDir(directory).canonicalPath();
    if (path.isEmpty()) path = QDir(directory).absolutePath();
#ifdef _WIN32
    path = path.toLower();
#endif
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("mw-router-holes\n"));
    hash.addData(machineId);
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(path.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(32));
}

std::unique_ptr<File> File::open(const QString& directory, bool devInstance,
                                 const QByteArray& machineId, const Earlier& earlier,
                                 const QString& lanNow, QString* said)
{
    const auto say = [&](const QString& what) {
        if (said) *said = what;
    };
    const QString base = QDir(directory).filePath(QStringLiteral("router_holes"));
    const intptr_t lock = lockFile(base + QStringLiteral(".lock"));
    if (lock == -1) {
        say(QStringLiteral("the record of this host's router entries is another process's, or "
                           "cannot be locked: this run keeps none"));
        return nullptr;
    }
    std::unique_ptr<File> file(new File);
    file->m_Lock = lock;
    file->m_Path = base + QStringLiteral(".json");
    const QString mine = fingerprint(machineId, directory);

    QFile on(file->m_Path);
    const bool there = on.exists();
    if (there) {
        if (!on.open(QIODevice::ReadOnly)) {
            // Not ours to replace on a read that failed: it may be whole
            say(QStringLiteral("the record of this host's router entries cannot be read: this "
                               "run keeps none"));
            return nullptr;
        }
        const QByteArray json = on.size() <= kFileMax ? on.readAll() : QByteArray();
        on.close();
        Content read;
        if (!json.isEmpty() && parse(json, devInstance, read) && read.owner == mine) {
            file->m_Content = read;
            return file;
        }
        say(QStringLiteral("the record of router entries found here is not this installation's, "
                           "or does not read as one: an empty one takes its place"));
    }
    file->m_Content.owner = mine;
    file->m_Content.tag = newTag();
    // Where no record was ever kept, what the settings remember stands for one
    if (!there) file->m_Content.holes = earlierHoles(devInstance, earlier, lanNow);
    file->save();
    return file;
}

File::~File()
{
    unlockFile(m_Lock);
}

bool File::find(uint16_t external, bool tcp, Hole& out) const
{
    for (const Hole& line : m_Content.holes) {
        if (line.external != external || line.tcp != tcp) continue;
        out = line;
        return true;
    }
    return false;
}

void File::note(const Hole& hole)
{
    // A router has one entry a number and protocol, and it is this line's
    // from now on: what the other kind noted of it is no more.
    bool changed = false;
    for (qsizetype i = 0; i < m_Content.others.size(); ++i) {
        const Hole& line = m_Content.others.at(i);
        if (line.external != hole.external || line.tcp != hole.tcp) continue;
        m_Content.others.removeAt(i);
        changed = true;
        break;
    }
    for (Hole& line : m_Content.holes) {
        if (line.external != hole.external || line.tcp != hole.tcp) continue;
        if (!(line == hole)) {
            line = hole;
            changed = true;
        }
        if (changed) save();
        return;
    }
    // Never reached by a host's own numbers: they are fewer than that
    if (m_Content.holes.size() + m_Content.others.size() < kHolesMax) {
        m_Content.holes.append(hole);
        changed = true;
    }
    if (changed) save();
}

void File::strike(uint16_t external, bool tcp)
{
    for (qsizetype i = 0; i < m_Content.holes.size(); ++i) {
        const Hole& line = m_Content.holes.at(i);
        if (line.external != external || line.tcp != tcp) continue;
        m_Content.holes.removeAt(i);
        save();
        return;
    }
}

void File::save()
{
    // A sibling temporary file, flushed to the disk, then renamed over this
    // one: the path never holds half a record (as settings.json is written).
    QSaveFile file(m_Path);
    const bool written =
        file.open(QIODevice::WriteOnly) && file.write(serialize(m_Content)) >= 0 && file.commit();
    if (written) {
        m_SaveFailed = false;
        return;
    }
    // Said once: what is on the router is still right, only the note of it is not kept
    if (!m_SaveFailed)
        qWarning().noquote() << "[UPNP] The record of this host's router entries cannot be written:"
                             << file.errorString();
    m_SaveFailed = true;
}

} // namespace mw::routerholes
