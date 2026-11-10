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

class QMimeData;

/// Whether the host clipboard's content asked to stay out of other programs'
/// hands. Password managers (KeePass, KeePassXC, Bitwarden, 1Password…) put
/// such marks next to the text they copy, and Windows' own clipboard history
/// and cloud clipboard honour them; the browser mirror of the shared
/// clipboard is one more of those readers, so it honours them too.
///
/// Plain QtCore: no clipboard access here — the caller hands in the
/// QMimeData it already holds, which keeps the rules testable on their own.
namespace ClipboardSensitivity {

/// The native clipboard format name behind a Qt MIME type. On Windows, Qt
/// lists a registered format it has no converter for as
/// `application/x-qt-windows-mime;value="<name>"`, optionally followed by
/// `;index=<n>`; anything else is returned unchanged.
QString nativeFormatName(const QString& mimeType);

/// True when @p data carries one of the marks that keep a copy private:
/// - Windows: `ExcludeClipboardContentFromMonitorProcessing` or
///   `Clipboard Viewer Ignore` present, or `CanIncludeInClipboardHistory` /
///   `CanUploadToCloudClipboard` set to a DWORD of 0. A DWORD that cannot be
///   read counts as 0: when unsure, the text stays on the host.
/// - KDE / KeePassXC on Linux: `x-kde-passwordManagerHint` set to `secret`.
bool markedPrivate(const QMimeData& data);

} // namespace ClipboardSensitivity
