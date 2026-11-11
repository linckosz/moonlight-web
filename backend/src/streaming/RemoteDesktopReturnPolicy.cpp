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

#include "RemoteDesktopReturnPolicy.h"

namespace RemoteDesktopReturnPolicy {

QString consoleReturnRefusal(const ConsoleReturnFacts& facts)
{
    // The gate on the privileged side too: a version that does not offer the
    // return never connects a session to the console, whoever asks.
    if (!kReturnOffered) return QStringLiteral("this version does not offer it");
    return consoleReturnChecks(facts);
}

QString consoleReturnChecks(const ConsoleReturnFacts& facts)
{
    if (!facts.callerIsThisExecutable) return QStringLiteral("the caller is not this executable");
    if (facts.callerSession == kNoSession)
        return QStringLiteral("the caller's session could not be read");
    // Session 0 is the services' own: nobody's desktop to put on a screen.
    if (facts.callerSession == 0) return QStringLiteral("the caller runs in session 0");
    if (facts.consoleSession == kNoSession)
        return QStringLiteral("there is no console session to return to");
    if (facts.callerSession == facts.consoleSession)
        return QStringLiteral("the caller is already on the console");
    if (facts.callerState != Disconnected)
        return QStringLiteral("the caller's session %1 is not disconnected (state %2)")
            .arg(facts.callerSession)
            .arg(facts.callerState);
    if (facts.consoleUserPresent)
        return QStringLiteral("somebody is signed in on the console (session %1)")
            .arg(facts.consoleSession);
    return {};
}

QString hideRefusal(const ConsoleReturnFacts& facts)
{
    if (!facts.callerIsThisExecutable) return QStringLiteral("the caller is not this executable");
    if (facts.callerSession == kNoSession || facts.callerSession == 0)
        return QStringLiteral("the caller's session could not be read, or is session 0");
    if (facts.callerSession != facts.consoleSession)
        return QStringLiteral("the caller's session %1 is not the console (%2)")
            .arg(facts.callerSession)
            .arg(facts.consoleSession);
    if (facts.callerState != Active)
        return QStringLiteral("the caller's session %1 is not active (state %2)")
            .arg(facts.callerSession)
            .arg(facts.callerState);
    return {};
}

bool Tracker::observe(const Observation& now)
{
    if (now.state == Active) {
        // Back on a screen, or on another Remote Desktop client: a fresh start.
        m_LastActiveRemote = now.remote;
        m_DisconnectedTicks = 0;
        m_GaveUp = false;
        m_Left = false;
        return m_Left;
    }
    if (now.state != Disconnected) {
        // Connected, ConnectQuery and the like: a client is on its way in.
        // Nothing to say, nothing to do, and the count starts over.
        m_DisconnectedTicks = 0;
        m_Left = false;
        return m_Left;
    }
    ++m_DisconnectedTicks;
    m_Left = m_LastActiveRemote;
    return m_Left;
}

QString Tracker::returnRefusal(bool allowed) const
{
    if (!m_Left) return QStringLiteral("this session was not left by Remote Desktop");
    if (!allowed)
        return QStringLiteral("bringing it back is turned off in the admin settings, or the "
                              "launcher service is not installed");
    if (m_GaveUp)
        return QStringLiteral("the last return could not lock it, so it was sent away again: "
                              "sign in on the PC");
    if (m_DisconnectedTicks < kSettleTicks)
        return QStringLiteral("Remote Desktop has only just left it: try again in a few seconds");
    return {};
}

bool returnOffered(bool leftByRemoteDesktop, bool allowed)
{
    return kReturnOffered && leftByRemoteDesktop && allowed;
}

QString requestRefusal(const Tracker& tracker, bool allowed)
{
    if (!kReturnOffered) return QStringLiteral("this version does not offer it");
    return tracker.returnRefusal(allowed);
}

QString Tracker::signInRefusal(bool allowed) const
{
    if (!m_Left) return QStringLiteral("this session was not left by Remote Desktop");
    if (!allowed)
        return QStringLiteral("the native host is disabled, or the launcher service is not "
                              "installed");
    if (m_DisconnectedTicks < kSettleTicks)
        return QStringLiteral("Remote Desktop has only just left it: try again in a few seconds");
    return {};
}

QString signInLaunchRefusal(const SignInFacts& facts)
{
    // The gate on the privileged side too: a version that does not offer the
    // sign-in screen never starts a worker on a console nobody is signed in
    // on, whoever asks.
    if (!kSignInOffered) return QStringLiteral("this version does not offer it");
    return signInLaunchChecks(facts);
}

QString signInLaunchChecks(const SignInFacts& facts)
{
    if (!facts.callerIsThisExecutable) return QStringLiteral("the caller is not this executable");
    if (facts.callerSession == kNoSession)
        return QStringLiteral("the caller's session could not be read");
    // Session 0 is the services' own: no user's session left by anybody.
    if (facts.callerSession == 0) return QStringLiteral("the caller runs in session 0");
    if (facts.callerState != Disconnected)
        return QStringLiteral("the caller's session %1 is not disconnected (state %2)")
            .arg(facts.callerSession)
            .arg(facts.callerState);
    if (facts.consoleSession == kNoSession || facts.consoleSession == 0)
        return QStringLiteral("there is no console session to stream");
    if (facts.consoleSession == facts.callerSession)
        return QStringLiteral("the caller is on the console");
    if (facts.consoleUserPresent)
        return QStringLiteral("somebody is signed in on the console (session %1)")
            .arg(facts.consoleSession);
    if (facts.signInWorkerAlive)
        return QStringLiteral("the sign-in screen is already being streamed");
    return {};
}

bool signInOffered(bool leftByRemoteDesktop, bool allowed)
{
    return kSignInOffered && leftByRemoteDesktop && allowed;
}

QString signInRequestRefusal(const Tracker& tracker, bool allowed, bool booth)
{
    if (!kSignInOffered) return QStringLiteral("this version does not offer it");
    return signInRequestChecks(tracker, allowed, booth);
}

QString signInRequestChecks(const Tracker& tracker, bool allowed, bool booth)
{
    // The sign-in screen has the PC's power button and its other accounts'
    // tiles: what a person at the PC has, never a kiosk's.
    if (booth) return QStringLiteral("not from a kiosk");
    return tracker.signInRefusal(allowed);
}

bool guestMayStream(bool nativeHost, int appId)
{
    return !(nativeHost && appId == kSignInAppId);
}

} // namespace RemoteDesktopReturnPolicy
