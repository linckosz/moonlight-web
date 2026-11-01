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

#include "AudioExcludeShm.h"

#include "Edition.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#endif

namespace AudioExcludeShm {

#ifdef Q_OS_WIN

namespace {

constexpr quint32 kMagic = 0x4D574145; // "MWAE"
constexpr size_t kDataBytes = 16 * 1024;

struct Section
{
    quint32 magic;
    volatile LONG sequence;
    quint32 length;
    char data[kDataBytes];
};

std::wstring sectionName()
{
    // Per edition: a DEV install and the production one never read each
    // other's list. Local: the server and its workers share the console
    // session, whatever account each runs as.
    return L"Local\\" + mw::edition::productName().toStdWString() + L"-AudioExclude-v1";
}

HANDLE g_Mapping = nullptr;
Section* g_Section = nullptr;

} // namespace

void publish(const QStringList& exeNames)
{
    if (!g_Section) {
        g_Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(sizeof(Section)), sectionName().c_str());
        if (!g_Mapping) return;
        g_Section = static_cast<Section*>(
            MapViewOfFile(g_Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Section)));
        if (!g_Section) {
            CloseHandle(g_Mapping);
            g_Mapping = nullptr;
            return;
        }
        g_Section->magic = kMagic;
    }
    QByteArray bytes = exeNames.join(QLatin1Char('\n')).toUtf8();
    if (bytes.size() > static_cast<qsizetype>(kDataBytes)) bytes.truncate(kDataBytes);

    // Seqlock: odd while the bytes move, so a reader retries rather than
    // taking half a list.
    InterlockedIncrement(&g_Section->sequence);
    MemoryBarrier();
    g_Section->length = static_cast<quint32>(bytes.size());
    std::memcpy(g_Section->data, bytes.constData(), static_cast<size_t>(bytes.size()));
    MemoryBarrier();
    InterlockedIncrement(&g_Section->sequence);
}

bool read(QStringList* exeNames, quint32* sequence)
{
    static HANDLE s_Mapping = nullptr;
    static const Section* s_Section = nullptr;
    if (!s_Section) {
        s_Mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName().c_str());
        if (!s_Mapping) return false;
        s_Section = static_cast<const Section*>(
            MapViewOfFile(s_Mapping, FILE_MAP_READ, 0, 0, sizeof(Section)));
        if (!s_Section) {
            CloseHandle(s_Mapping);
            s_Mapping = nullptr;
            return false;
        }
    }
    if (s_Section->magic != kMagic) return false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = s_Section->sequence;
        if (before & 1) {
            Sleep(0);
            continue;
        }
        MemoryBarrier();
        const quint32 length = std::min<quint32>(s_Section->length, kDataBytes);
        QByteArray bytes(s_Section->data, static_cast<qsizetype>(length));
        MemoryBarrier();
        if (s_Section->sequence != before) continue;
        if (exeNames) {
            exeNames->clear();
            for (const QString& name :
                 QString::fromUtf8(bytes).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                exeNames->append(name.trimmed().toLower());
        }
        if (sequence) *sequence = static_cast<quint32>(before);
        return true;
    }
    return false;
}

#else

void publish(const QStringList&) {}

bool read(QStringList*, quint32*)
{
    return false;
}

#endif

} // namespace AudioExcludeShm
