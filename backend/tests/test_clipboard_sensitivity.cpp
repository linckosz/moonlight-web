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
#include <QList>
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

/// A clipboard a test scripts look by look: each call to mimeData() takes the
/// next entry (the last one repeats), and the counter moves when told to.
struct ScriptedClipboard
{
    QList<QMimeData*> looks;
    int mimeCalls = 0;
    int textCalls = 0;
    quint32 seq = 7;
    /// Bumps the counter while the text is being read (another write lands).
    bool moveDuringText = false;

    ClipboardSensitivity::HostCopySource source()
    {
        return {
            [this]() { return seq; },
            [this]() -> const QMimeData* {
                if (looks.isEmpty()) return nullptr;
                const int i = qMin(mimeCalls++, int(looks.size()) - 1);
                return looks[i];
            },
            [this]() {
                ++textCalls;
                if (moveDuringText) ++seq;
                return QStringLiteral("some copied text");
            },
        };
    }
};

void run_host_copy_read_tests()
{
    using ClipboardSensitivity::HostCopy;
    using ClipboardSensitivity::readHostCopy;

    QMimeData plain;
    plain.setText(QStringLiteral("some copied text"));
    QMimeData marked;
    marked.setText(QStringLiteral("some copied text"));
    marked.setData(windowsFormat("CanIncludeInClipboardHistory"), dword(0));

    SECTION("ClipboardSensitivity: a settled ordinary copy is read");
    {
        ScriptedClipboard cb;
        cb.looks = {&plain};
        const auto read = readHostCopy(cb.source());
        CHECK(read.kind == HostCopy::Text);
        CHECK_EQ(read.text, QStringLiteral("some copied text"));
        CHECK_EQ(cb.mimeCalls, 2);
    }

    SECTION("ClipboardSensitivity: marked at the first look, the text is never read");
    {
        ScriptedClipboard cb;
        cb.looks = {&marked};
        const auto read = readHostCopy(cb.source());
        CHECK(read.kind == HostCopy::Private);
        CHECK(read.text.isEmpty());
        CHECK_EQ(cb.textCalls, 0);
    }

    SECTION("ClipboardSensitivity: text first, mark a moment later (two writes)");
    {
        ScriptedClipboard cb;
        cb.looks = {&plain, &marked};
        const auto read = readHostCopy(cb.source());
        CHECK(read.kind == HostCopy::Private);
        CHECK(read.text.isEmpty());
    }

    SECTION("ClipboardSensitivity: a write while reading sends nothing");
    {
        ScriptedClipboard cb;
        cb.looks = {&plain};
        cb.moveDuringText = true;
        const auto read = readHostCopy(cb.source());
        CHECK(read.kind == HostCopy::Changed);
        CHECK(read.text.isEmpty());
    }

    SECTION("ClipboardSensitivity: an empty clipboard reads as no mark");
    {
        ScriptedClipboard cb;
        const auto read = readHostCopy(cb.source());
        CHECK(read.kind == HostCopy::Text);
    }
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

    SECTION("ClipboardSensitivity: a format named with an index");
    {
        const QString indexed = QStringLiteral(
            "application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\";index=0");
        CHECK(privateWith(indexed, dword(0)));
        CHECK(!privateWith(indexed, dword(1)));
        CHECK(
            privateWith(QStringLiteral("application/x-qt-windows-mime;value="
                                       "\"ExcludeClipboardContentFromMonitorProcessing\";index=2"),
                        QByteArray()));
    }

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

    run_host_copy_read_tests();
}
