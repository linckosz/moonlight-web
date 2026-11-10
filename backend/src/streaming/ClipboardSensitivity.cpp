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

#include "ClipboardSensitivity.h"

#include <QByteArray>
#include <QMimeData>
#include <QStringList>

namespace {

// Qt's prefix for a Windows clipboard format it has no converter for
// (qwindowsmimeregistry.cpp, QLastResortMimes::mimeForFormat).
constexpr QLatin1StringView kQtWindowsMimePrefix("application/x-qt-windows-mime;value=\"");

// Present at all: the content is not to be read by clipboard monitors.
constexpr QLatin1StringView kExcludeFromMonitors("ExcludeClipboardContentFromMonitorProcessing");
constexpr QLatin1StringView kViewerIgnore("Clipboard Viewer Ignore");

// A DWORD: 0 keeps the content out of Windows' history, or its cloud sync.
constexpr QLatin1StringView kCanIncludeInHistory("CanIncludeInClipboardHistory");
constexpr QLatin1StringView kCanUploadToCloud("CanUploadToCloudClipboard");

// KDE's hint, which KeePassXC sets on Linux; "secret" keeps it out of Klipper.
constexpr QLatin1StringView kKdePasswordHint("x-kde-passwordManagerHint");

bool sameName(const QString& a, QLatin1StringView b)
{
    // Windows clipboard format names are matched without regard to case.
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

/// A clipboard DWORD set to 0, or one that cannot be read.
bool dwordIsZero(const QByteArray& bytes)
{
    if (bytes.size() < 4) return true;
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.constData());
    const quint32 value =
        quint32(b[0]) | (quint32(b[1]) << 8) | (quint32(b[2]) << 16) | (quint32(b[3]) << 24);
    return value == 0;
}

} // namespace

namespace ClipboardSensitivity {

QString nativeFormatName(const QString& mimeType)
{
    if (!mimeType.startsWith(kQtWindowsMimePrefix, Qt::CaseInsensitive)) return mimeType;
    const qsizetype start = kQtWindowsMimePrefix.size();
    const qsizetype end = mimeType.indexOf(u'"', start);
    return end < 0 ? mimeType.mid(start) : mimeType.mid(start, end - start);
}

bool markedPrivate(const QMimeData& data)
{
    const QStringList formats = data.formats();
    for (const QString& format : formats) {
        const QString name = nativeFormatName(format);
        if (sameName(name, kExcludeFromMonitors) || sameName(name, kViewerIgnore)) return true;
        if (sameName(name, kCanIncludeInHistory) || sameName(name, kCanUploadToCloud)) {
            if (dwordIsZero(data.data(format))) return true;
            continue;
        }
        if (sameName(name, kKdePasswordHint) && data.data(format).trimmed() == "secret")
            return true;
    }
    return false;
}

} // namespace ClipboardSensitivity
