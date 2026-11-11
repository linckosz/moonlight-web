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
#ifdef Q_OS_MACOS
#include "ClipboardMac.h"
#endif
#include "IMediaEngine.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaMethod>
#include <QNetworkInterface>
#include <QPointer>
#include <QtDebug>
#include <QTimer>

namespace {
constexpr bool kMacHost =
#ifdef Q_OS_MACOS
    true;
#else
    false;
#endif

QString clipboardText()
{
#ifdef Q_OS_MACOS
    // Qt detects external macOS clipboard changes on application activation;
    // the stream worker stays in the background while another app owns focus.
    return macClipboardText();
#else
    return QGuiApplication::clipboard()->text();
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
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            &ClipboardBridge::onClipboardChanged);
#ifdef Q_OS_MACOS
    // Poll changeCount while backgrounded; read text only when it changes.
    auto* pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &ClipboardBridge::onClipboardChanged);
    pollTimer->start(250);
#endif
    qInfo() << "[ClipboardBridge] Created (host clipboard monitor active)";
}

const char* ClipboardBridge::capabilitiesMessage()
{
    if (kMacHost)
        return "{\"type\":\"clipboardcaps\",\"available\":true,"
               "\"pasteModifier\":\"meta\"}";
    return "{\"type\":\"clipboardcaps\",\"available\":true,"
           "\"pasteModifier\":\"control\"}";
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
            if (!shimGuard) return;
            const QString t = text.left(kMaxTextChars);
            if (!t.isEmpty()) {
                const bool needsWrite = t != clipboardText();
                // Suppress an echo even when the host already has this text.
                m_LastText = t;
                if (needsWrite) QGuiApplication::clipboard()->setText(t);
            }
            // Inject the chord only AFTER the clipboard is committed — same
            // main-thread hop, so ordering is structural. Li input calls are
            // thread-safe (guarded by the shim's connected atomic).
            IMediaEngine* s = shimGuard.data();
            if (!s) return;
            // This host-generated chord bypasses browser key mappings.
            // VK_LWIN is Command on macOS; other hosts paste with Control.
            const short modifier = kMacHost ? 0x5B : 0x11;
            const char flags = kMacHost ? 0x08 : 0x02;
            const bool wrap = kMacHost || wrapCtrl;
            if (wrap) s->sendKeyEvent(modifier, true, flags, 0);
            s->sendKeyEvent(0x56, true, flags, 0);
            s->sendKeyEvent(0x56, false, flags, 0);
            if (wrap) s->sendKeyEvent(modifier, false, 0, 0);
        },
        Qt::QueuedConnection);
}

void ClipboardBridge::requestAnnounce()
{
    QMetaObject::invokeMethod(
        this,
        [this]() {
            const QString text = clipboardText().left(kMaxTextChars);
            if (text.isEmpty()) return;
            m_LastText = text;
            emit hostTextChanged(text);
        },
        Qt::QueuedConnection);
}

void ClipboardBridge::onClipboardChanged()
{
    // Don't even read the clipboard while no session is listening.
    static const QMetaMethod changedSignal =
        QMetaMethod::fromSignal(&ClipboardBridge::hostTextChanged);
    if (!isSignalConnected(changedSignal)) return;
#ifdef Q_OS_MACOS
    const qint64 changeCount = macClipboardChangeCount();
    if (changeCount == m_ChangeCount) return;
    m_ChangeCount = changeCount;
#endif

    const QString text = clipboardText().left(kMaxTextChars);
    // Empty = cleared or non-text content (image/files) — text-only bridge.
    // Identical text = echo of our own setText() or a no-op copy.
    if (text.isEmpty() || text == m_LastText) return;

    m_LastText = text;
    emit hostTextChanged(text);
}
