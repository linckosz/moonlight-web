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

// The marks a program puts next to a copy it wants kept private, read off a
// QMimeData built here: the real clipboard is never touched.

#include "test_framework.h"
#include "streaming/ClipboardSensitivity.h"

#include <QByteArray>
#include <QMimeData>

namespace {

/// The MIME type Qt gives a Windows clipboard format it has no converter for.
QString windowsFormat(const QString& name)
{
    return QStringLiteral("application/x-qt-windows-mime;value=\"%1\"").arg(name);
}

QByteArray dword(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF);
    b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF);
    b[3] = char((v >> 24) & 0xFF);
    return b;
}

/// A text copy, plus one extra format when @p format is not empty.
bool privateWith(const QString& format, const QByteArray& value)
{
    QMimeData data;
    data.setText(QStringLiteral("some copied text"));
    if (!format.isEmpty()) data.setData(format, value);
    return ClipboardSensitivity::markedPrivate(data);
}

} // namespace

void run_clipboard_sensitivity_tests()
{
    using ClipboardSensitivity::nativeFormatName;

    SECTION("ClipboardSensitivity: Windows format names behind Qt's MIME types");
    CHECK_EQ(nativeFormatName(windowsFormat("CanIncludeInClipboardHistory")),
             QStringLiteral("CanIncludeInClipboardHistory"));
    CHECK_EQ(nativeFormatName(QStringLiteral(
                 "application/x-qt-windows-mime;value=\"Clipboard Viewer Ignore\";index=0")),
             QStringLiteral("Clipboard Viewer Ignore"));
    CHECK_EQ(nativeFormatName(QStringLiteral("text/plain")), QStringLiteral("text/plain"));

    SECTION("ClipboardSensitivity: an ordinary copy goes out");
    CHECK(!privateWith(QString(), QByteArray()));
    CHECK(!privateWith(QStringLiteral("text/html"), QByteArray("<b>x</b>")));
    CHECK(!privateWith(windowsFormat("Rich Text Format"), QByteArray("{\\rtf1}")));

    SECTION("ClipboardSensitivity: a monitor exclusion keeps it on the host");
    CHECK(privateWith(windowsFormat("ExcludeClipboardContentFromMonitorProcessing"), QByteArray()));
    CHECK(privateWith(windowsFormat("ExcludeClipboardContentFromMonitorProcessing"),
                      QByteArray(1, '\0')));
    CHECK(privateWith(windowsFormat("Clipboard Viewer Ignore"), QByteArray()));
    // Windows matches format names without regard to case.
    CHECK(privateWith(windowsFormat("excludeclipboardcontentfrommonitorprocessing"), QByteArray()));

    SECTION("ClipboardSensitivity: history and cloud flags, read as a DWORD");
    CHECK(privateWith(windowsFormat("CanIncludeInClipboardHistory"), dword(0)));
    CHECK(privateWith(windowsFormat("CanUploadToCloudClipboard"), dword(0)));
    CHECK(!privateWith(windowsFormat("CanIncludeInClipboardHistory"), dword(1)));
    CHECK(!privateWith(windowsFormat("CanUploadToCloudClipboard"), dword(1)));
    // Any non-zero value allows it, whichever byte holds it.
    CHECK(!privateWith(windowsFormat("CanIncludeInClipboardHistory"), dword(0x100)));
    // A value that cannot be read: kept on the host.
    CHECK(privateWith(windowsFormat("CanIncludeInClipboardHistory"), QByteArray(2, '\0')));
    CHECK(privateWith(windowsFormat("CanUploadToCloudClipboard"), QByteArray()));

    SECTION("ClipboardSensitivity: allowed to history but excluded from the cloud");
    {
        QMimeData data;
        data.setText(QStringLiteral("some copied text"));
        data.setData(windowsFormat("CanIncludeInClipboardHistory"), dword(1));
        data.setData(windowsFormat("CanUploadToCloudClipboard"), dword(0));
        CHECK(ClipboardSensitivity::markedPrivate(data));
    }

    SECTION("ClipboardSensitivity: KDE's password manager hint");
    CHECK(privateWith(QStringLiteral("x-kde-passwordManagerHint"), QByteArray("secret")));
    CHECK(!privateWith(QStringLiteral("x-kde-passwordManagerHint"), QByteArray("other")));
}
