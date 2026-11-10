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

#include "ClipboardBridge.h"
#include "ClipboardSensitivity.h"
#include "IMediaEngine.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaMethod>
#include <QMimeData>
#include <QNetworkInterface>
#include <QPointer>
#include <QTimer>
#include <QtDebug>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// Windows virtual-key codes for the injected paste chord (the Moonlight
// protocol speaks VK regardless of the host OS — Sunshine translates).
constexpr short kVkControl = 0x11;
constexpr short kVkV = 0x56;
// Modifier bitmask matching the relays' keydown encoding (Ctrl = 0x02).
constexpr char kModCtrl = 0x02;

/// The clipboard's change counter. Without one (macOS, Linux), the second
/// look at the marks makes the check on its own.
quint32 clipboardSequence()
{
#ifdef Q_OS_WIN
    return static_cast<quint32>(GetClipboardSequenceNumber());
#else
    return 0;
#endif
}
} // namespace

ClipboardBridge* ClipboardBridge::instance()
{
    // Parented to the app so it dies with the main event loop. Thread-safe
    // static init; first caller's thread becomes the object's thread — see
    // header contract (first call is from Session on the main thread).
    static ClipboardBridge* s_Instance = new ClipboardBridge(QCoreApplication::instance());
    return s_Instance;
}

ClipboardBridge::ClipboardBridge(QObject* parent)
    : QObject(parent)
    , m_Settle(new QTimer(this))
{
    m_Settle->setSingleShot(true);
    m_Settle->setInterval(kSettleMs);
    connect(m_Settle, &QTimer::timeout, this, &ClipboardBridge::relaySettledCopy);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            &ClipboardBridge::onClipboardChanged);
    qInfo() << "[ClipboardBridge] Created (host clipboard monitor active)";
}

bool ClipboardBridge::isSelfAddress(const QString& address)
{
    if (address.isEmpty()) return false;
    if (address.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return true;
    if (address.compare(QHostInfo::localHostName(), Qt::CaseInsensitive) == 0) return true;

    const QHostAddress addr(address);
    if (addr.isNull()) return false; // hostname we can't cheaply resolve — assume remote
    if (addr.isLoopback()) return true;

    const QList<QHostAddress> locals = QNetworkInterface::allAddresses();
    for (const QHostAddress& local : locals) {
        if (addr.isEqual(local, QHostAddress::TolerantConversion)) return true;
    }
    return false;
}

void ClipboardBridge::pasteFromClient(IMediaEngine* engine, const QString& text, bool wrapCtrl)
{
    // The shim is Qt-parented to its relay and dies on the relay thread; the
    // lambda runs on the main thread — guard with a QPointer so a teardown
    // racing this queued call degrades to a no-op instead of a dangling call.
    QPointer<IMediaEngine> shimGuard(engine);
    QMetaObject::invokeMethod(
        this,
        [this, shimGuard, text, wrapCtrl]() {
            const QString t = text.left(kMaxTextChars);
            if (!t.isEmpty() && t != m_LastText) {
                m_LastText = t;
                QGuiApplication::clipboard()->setText(t);
            }
            // Inject the chord only AFTER the clipboard is committed — same
            // main-thread hop, so ordering is structural. Li input calls are
            // thread-safe (guarded by the shim's connected atomic).
            IMediaEngine* s = shimGuard.data();
            if (!s) return;
            if (wrapCtrl) s->sendKeyEvent(kVkControl, true, kModCtrl, 0);
            s->sendKeyEvent(kVkV, true, kModCtrl, 0);
            s->sendKeyEvent(kVkV, false, kModCtrl, 0);
            if (wrapCtrl) s->sendKeyEvent(kVkControl, false, 0, 0);
        },
        Qt::QueuedConnection);
}

void ClipboardBridge::requestAnnounce()
{
    QMetaObject::invokeMethod(
        this,
        [this]() {
            // Through the same settle and checks as a change, so a copy
            // still being written is not sent at connect time either.
            m_AnnounceDue = true;
            m_Settle->start();
        },
        Qt::QueuedConnection);
}

bool ClipboardBridge::hasListener() const
{
    static const QMetaMethod changedSignal =
        QMetaMethod::fromSignal(&ClipboardBridge::hostTextChanged);
    return isSignalConnected(changedSignal);
}

void ClipboardBridge::onClipboardChanged()
{
    // Don't even read the clipboard while no session is listening.
    if (!hasListener()) return;
    // Restarted by every change: the copy is read once it has stopped moving.
    m_Rereads = 0;
    m_Settle->start();
}

void ClipboardBridge::relaySettledCopy()
{
    if (!hasListener()) {
        m_AnnounceDue = false;
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    const ClipboardSensitivity::HostCopyRead read = ClipboardSensitivity::readHostCopy({
        &clipboardSequence,
        [clipboard]() { return clipboard->mimeData(); },
        [clipboard]() { return clipboard->text(); },
    });

    switch (read.kind) {
    case ClipboardSensitivity::HostCopy::Changed:
        // A new write raises its own notification, which reads again (an
        // announce still due stays due). Read again here too, a bounded
        // number of times: a program that renders its text only when asked
        // may move the counter during our own read, with no notification.
        if (m_Rereads < kMaxRereads) {
            ++m_Rereads;
            m_Settle->start();
        }
        return;
    case ClipboardSensitivity::HostCopy::Private:
        // Forgotten as well: the next ordinary copy goes out even if its
        // text matches what was relayed before this one.
        m_LastText.clear();
        m_AnnounceDue = false;
        m_Rereads = 0;
        qInfo() << "[ClipboardBridge] Host copy kept on the host: its owner marked it private";
        return;
    case ClipboardSensitivity::HostCopy::Text: break;
    }

    const bool announce = m_AnnounceDue;
    m_AnnounceDue = false;
    m_Rereads = 0;
    const QString text = read.text.left(kMaxTextChars);
    // Empty = cleared or non-text content (image/files) — text-only bridge.
    // Identical text = echo of our own setText() or a no-op copy, unless a
    // newly connected client asked for the current text.
    if (text.isEmpty() || (!announce && text == m_LastText)) return;

    m_LastText = text;
    emit hostTextChanged(text);
}
