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

#include "InputWatchdog.h"

#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QTimer>

InputWatchdog::InputWatchdog(Sink sink, QObject* parent)
    : QObject(parent)
    , m_Sink(std::move(sink))
{
    m_Clock.start();
    m_Now = [this]() { return m_Clock.elapsed(); };
    // Created here, on the owning thread, so it has the right affinity from
    // the start; the feeds only ever start and stop it through arm().
    m_Timer = new QTimer(this);
    m_Timer->setTimerType(Qt::CoarseTimer);
    connect(m_Timer, &QTimer::timeout, this, &InputWatchdog::tick);
}

// --- Feeds -------------------------------------------------------------------

void InputWatchdog::noteKey(const HeldKey& key, bool down)
{
    {
        QMutexLocker lock(&m_Mutex);
        if (down)
            m_HeldKeys.insert(key.keyCode, key);
        else
            m_HeldKeys.remove(key.keyCode);
    }
    arm();
}

void InputWatchdog::noteButton(int button, bool down, bool hold)
{
    const quint32 bit = (button >= 1 && button <= 32) ? (1u << (button - 1)) : 0u;
    {
        QMutexLocker lock(&m_Mutex);
        if (down) {
            m_HeldButtons |= bit;
            if (hold) m_HeldButtonsHold = true;
        } else {
            m_HeldButtons &= ~bit;
            if (m_HeldButtons == 0) m_HeldButtonsHold = false;
        }
    }
    arm();
}

void InputWatchdog::notePad(short controller, short mask, bool atRest)
{
    {
        QMutexLocker lock(&m_Mutex);
        if (atRest)
            m_ActivePads.remove(controller);
        else
            m_ActivePads.insert(controller, mask);
    }
    arm();
}

bool InputWatchdog::padAtRest(int buttonFlags, unsigned char leftTrigger,
                              unsigned char rightTrigger, short leftStickX, short leftStickY,
                              short rightStickX, short rightStickY)
{
    static constexpr short kStickAtRest = 4096;
    return buttonFlags == 0 && leftTrigger == 0 && rightTrigger == 0 &&
           qAbs(leftStickX) < kStickAtRest && qAbs(leftStickY) < kStickAtRest &&
           qAbs(rightStickX) < kStickAtRest && qAbs(rightStickY) < kStickAtRest;
}

void InputWatchdog::noteClientAlive()
{
    QMutexLocker lock(&m_Mutex);
    m_LastAliveMs = m_Now();
    m_ShortFired = false;
    m_LongFired = false;
}

bool InputWatchdog::anythingHeld() const
{
    QMutexLocker lock(&m_Mutex);
    return anythingHeldLocked();
}

// --- Timer -------------------------------------------------------------------

void InputWatchdog::arm()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { arm(); }, Qt::QueuedConnection);
        return;
    }

    bool run;
    {
        QMutexLocker lock(&m_Mutex);
        run = m_HeartbeatSeen && anythingHeldLocked();
    }
    if (!run) {
        m_Timer->stop();
        return;
    }
    if (!m_Timer->isActive()) m_Timer->start(kTickMs);
}

void InputWatchdog::tick()
{
    qint64 silentMs;
    bool longFire = false;
    bool shortFire = false;
    {
        QMutexLocker lock(&m_Mutex);
        // The timer only runs past the handshake; a tick driven by hand (the
        // tests) is held to the same rule.
        if (!m_HeartbeatSeen || m_LastAliveMs < 0) return;
        silentMs = m_Now() - m_LastAliveMs;
        if (silentMs >= kStaleHoldMs && !m_LongFired) {
            m_LongFired = m_ShortFired = longFire = true;
        } else if (silentMs >= kStaleMs && !m_ShortFired) {
            m_ShortFired = shortFire = true;
        }
    }

    if (longFire) {
        qWarning() << "[InputWatchdog] Input link silent for" << silentMs
                   << "ms — releasing every held input";
        release(true);
    } else if (shortFire) {
        qInfo() << "[InputWatchdog] Input link silent for" << silentMs
                << "ms — releasing held inputs (hold-flagged ones kept)";
        release(false);
    }
}

void InputWatchdog::setClock(std::function<qint64()> nowMs)
{
    QMutexLocker lock(&m_Mutex);
    m_Now = std::move(nowMs);
}

// --- Release and resync ------------------------------------------------------

void InputWatchdog::release(bool includeHold)
{
    QVector<HeldKey> keys;
    quint32 buttons = 0;
    QHash<short, short> pads;
    {
        QMutexLocker lock(&m_Mutex);
        for (auto it = m_HeldKeys.begin(); it != m_HeldKeys.end();) {
            if (it->hold && !includeHold) {
                ++it;
                continue;
            }
            keys.append(it.value());
            it = m_HeldKeys.erase(it);
        }
        if (m_HeldButtons != 0 && (includeHold || !m_HeldButtonsHold)) {
            buttons = m_HeldButtons;
            m_HeldButtons = 0;
            m_HeldButtonsHold = false;
        }
        // Gamepads are gaming input by definition: only the long grace period
        // (link presumed dead) neutralizes them.
        if (includeHold) {
            pads = m_ActivePads;
            m_ActivePads.clear();
        }
    }

    // Modifiers cleared on release, like the client's own focus-loss path: the
    // modifier keys are in this same set and are released alongside.
    if (m_Sink.releaseKey) {
        for (const HeldKey& k : keys)
            m_Sink.releaseKey(k);
    }
    if (m_Sink.releaseButton) {
        for (int b = 1; b <= 32; ++b) {
            if (buttons & (1u << (b - 1))) m_Sink.releaseButton(b);
        }
    }
    if (m_Sink.neutralizePad) {
        for (auto it = pads.constBegin(); it != pads.constEnd(); ++it)
            m_Sink.neutralizePad(it.key(), it.value());
    }

    arm();
}

InputWatchdog::SyncDiff InputWatchdog::sync(const QVector<HeldKey>& keys, quint32 buttonMask,
                                            bool buttonsHold)
{
    SyncDiff diff;
    {
        QMutexLocker lock(&m_Mutex);
        m_HeartbeatSeen = true;

        QSet<short> clientKeys;
        clientKeys.reserve(keys.size());
        for (const HeldKey& k : keys) {
            clientKeys.insert(k.keyCode);
            auto it = m_HeldKeys.find(k.keyCode);
            if (it == m_HeldKeys.end()) {
                m_HeldKeys.insert(k.keyCode, k);
                diff.press.append(k);
            } else {
                it->hold = k.hold; // mouse mode may have flipped mid-hold
            }
        }
        for (auto it = m_HeldKeys.begin(); it != m_HeldKeys.end();) {
            if (clientKeys.contains(it.key())) {
                ++it;
                continue;
            }
            diff.release.append(it.value());
            it = m_HeldKeys.erase(it);
        }

        for (int b = 1; b <= 32; ++b) {
            const quint32 bit = 1u << (b - 1);
            if ((buttonMask & bit) && !(m_HeldButtons & bit))
                diff.buttonsDown.append(b);
            else if (!(buttonMask & bit) && (m_HeldButtons & bit))
                diff.buttonsUp.append(b);
        }
        m_HeldButtons = buttonMask;
        m_HeldButtonsHold = buttonMask != 0 && buttonsHold;
    }

    arm();
    return diff;
}
