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
class QTimer;

/// Bidirectional text clipboard bridge between the browser client and the
/// host machine, active only when the streamed Sunshine host IS this machine
/// (the standard installer deployment) — the backend process then shares the
/// host's clipboard and QClipboard is authoritative.
///
/// Local → host: the browser intercepts Ctrl/Cmd+V via its native 'paste'
/// event (the only permission-free clipboard read) and sends a
/// 'clipboardpaste' message; pasteFromClient() commits the text to the host
/// clipboard and injects the paste chord in the same main-thread hop, so the
/// keystrokes can never overtake the clipboard write.
///
/// Host → local: QClipboard::dataChanged is monitored on the main thread and
/// re-emitted as hostTextChanged(); relays forward it to the browser, which
/// mirrors it via navigator.clipboard.writeText(). A copy is read once the
/// clipboard has been still for kSettleMs, and one marked private by the
/// program that made it (ClipboardSensitivity) stays on the host.
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

    /// How long the host clipboard must stay unchanged before a copy is read:
    /// a program may write its text and add its marks in two steps.
    static constexpr int kSettleMs = 200;
    /// Reads retried when the clipboard changed while being read.
    static constexpr int kMaxRereads = 3;

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

    /// Thread-safe. Queue onto the main thread: commit 'text' to the host
    /// clipboard, then inject the paste chord (V down/up) through the shim.
    /// wrapCtrl adds Ctrl down/up around the V for clients whose paste
    /// modifier doesn't map to Ctrl on the host (macOS Cmd arrives as Win).
    void pasteFromClient(IMediaEngine* engine, const QString& text, bool wrapCtrl);

    /// Thread-safe. Re-emit the current host clipboard text (initial sync
    /// when a client connects), even if unchanged since the last emission,
    /// after the same settle and checks as a change.
    void requestAnnounce();

signals:
    /// Host clipboard changed (or an announce was requested). Emitted on the
    /// main thread; relays connect with themselves as context so the slot
    /// runs on their thread.
    void hostTextChanged(const QString& text);

private:
    explicit ClipboardBridge(QObject* parent);
    void onClipboardChanged();
    /// Read the settled host copy and relay it, unless it is marked private
    /// or changed while being read. Main thread only.
    void relaySettledCopy();
    bool hasListener() const;

    /// Restarted by each change and by requestAnnounce().
    QTimer* m_Settle = nullptr;
    /// A client asked for the current text: send it even if unchanged.
    bool m_AnnounceDue = false;
    /// Reads retried since the last settled one (bounded by kMaxRereads).
    int m_Rereads = 0;

    /// Last text written or observed — suppresses the dataChanged echo of our
    /// own setText() and redundant re-pushes of an unchanged clipboard.
    QString m_LastText;
};
