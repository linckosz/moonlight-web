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

#include <QObject>
#include <QString>

class IMediaEngine;

/// Bidirectional text clipboard bridge between the browser client and the
/// host machine, for unrestricted native-host or local Sunshine sessions.
/// Eligibility is decided by Session; the backend shares the host clipboard.
///
/// Local → host: native 'paste' events or an async read for mapped shortcuts
/// supply text. pasteFromClient() writes it before injecting the host chord.
///
/// Host → local: QClipboard notifications, plus macOS changeCount polling,
/// emit hostTextChanged(); relays forward the text for browser clipboard writes.
///
/// All QClipboard access happens on the Qt main thread (QClipboard is
/// main-thread-only); relays call in from their dedicated threads through
/// queued invocations.
class ClipboardBridge : public QObject
{
    Q_OBJECT

public:
    /// Max text size relayed in either direction (UTF-16 code units). Larger
    /// payloads are truncated: clipboard sync is for text, not file transfer,
    /// and input-channel messages must stay well under SCTP message limits.
    static constexpr int kMaxTextChars = 256 * 1024;

    /// Lazily created singleton. The FIRST call must come from the Qt main
    /// thread so the bridge gets main-thread affinity — guaranteed today:
    /// Session::setClipboardEnabled(true) runs on the main thread before any
    /// relay-thread code can reach the clipboard paths.
    static ClipboardBridge* instance();

    /// True when 'address' designates this machine (localhost, loopback IP,
    /// local hostname or an address bound to a local interface) — the only
    /// case where the backend clipboard is the streamed host's clipboard.
    /// Unresolvable hostnames are treated as remote (no blocking DNS lookup).
    static bool isSelfAddress(const QString& address);

    /// Shared relay announcement: clipboard availability and host paste modifier.
    static const char* capabilitiesMessage();

    /// Thread-safe. Queue onto the main thread: commit 'text' to the host
    /// clipboard, then inject the paste chord (V down/up) through the shim.
    /// wrapCtrl adds Ctrl down/up on non-Mac hosts when the client released it
    /// or used another modifier. Mac hosts always inject a complete Cmd+V.
    void pasteFromClient(IMediaEngine* engine, const QString& text, bool wrapCtrl);

    /// Thread-safe. Re-emit the current host clipboard text (initial sync
    /// when a client connects), even if unchanged since the last emission.
    void requestAnnounce();

signals:
    /// Host clipboard changed (or an announce was requested). Emitted on the
    /// main thread; relays connect with themselves as context so the slot
    /// runs on their thread.
    void hostTextChanged(const QString& text);

private:
    explicit ClipboardBridge(QObject* parent);
    void onClipboardChanged();

    /// Last text written or observed — suppresses the dataChanged echo of our
    /// own setText() and redundant re-pushes of an unchanged clipboard.
    QString m_LastText;
    qint64 m_ChangeCount = -1;
};
