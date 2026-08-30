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

/**
 * @brief Which operating system a host runs — worked out, never asked for.
 *
 * Why this exists at all
 * ---------------------
 * Exactly one behaviour depends on it, and it is not cosmetic: whether the host
 * keeps the part of a scroll amount that is not yet a whole 120-unit notch.
 *
 *   Windows  keeps it — SendInput accumulates leftovers, and libvirtualhid's
 *            Windows backend carries a `vertical_scroll_remainder_` besides.
 *   macOS    keeps it — libvirtualhid's macOS backend converts the high-res
 *            amount to CoreGraphics pixels (`scroll_pixels()`), multiplying by
 *            pixels-per-line and lines-per-detent BEFORE dividing by 120, so a
 *            sub-notch amount still moves the page.
 *   Linux    throws it away — inputtino's Mouse::vertical_scroll computes
 *            `REL_WHEEL = high_res_distance / 120` with no accumulator, so
 *            anything under a notch is worth zero clicks and survives only if
 *            the session consumes the REL_WHEEL_HI_RES companion event, which
 *            X11 sessions routinely do not.
 *
 * So the client must hold sub-notch amounts back for a Linux host and must NOT
 * for the other two: quantizing a Windows host turns a trackpad or a high-res
 * wheel into a ratchet. There is no single wire value that suits both — a host
 * that counts only whole notches needs events of at least 120, and smoothness
 * needs events well under it — which is why this has to be decided per host.
 *
 * Read `docs/upstream/inputtino-scroll-remainder.patch` for the fix that
 * removes the Linux side of this problem at the source; it is already the
 * behaviour of libvirtualhid, which Sunshine moved to in v2026.830.44125.
 *
 * What can be established, and what cannot
 * ---------------------------------------
 * Nothing in GameStream names the OS. `serverinfo` was checked field by field
 * (see BackendProbe.h for why its version strings are worthless), and so was
 * the Sunshine management API: an unauthenticated GET of `/`, `/api/config` and
 * `/welcome` on a live host all answer 401, and the one endpoint that does
 * answer, `/api/configLocale`, returns `{"locale":"en","status":true}`. There
 * is no OS field to read anywhere on the control plane.
 *
 * What is left is a handful of facts that each imply an OS on their own:
 *
 *  - The host resolves to THIS machine. Then its OS is ours, at compile time.
 *  - Its backend is proven to be Wolf. Wolf ships as Linux containers only.
 *  - A MultiSeat control API answers on it. MultiSeat drives Windows sessions.
 *  - It is the original NVIDIA software (state MJOLNIR), which was Windows-only.
 *  - The IP TTL of packets it sends us. Stacks start at 128 (Windows) or 64
 *    (Linux, macOS), so a TTL above 64 is Windows and one at or below it is
 *    Unix-like — which does NOT separate Linux from macOS, and those two want
 *    opposite treatment. So a low TTL deliberately proves nothing here.
 *
 * Everything else stays Unknown, and Unknown quantizes: a dropped notch is a
 * feature that does not work, while a chunky notch is one that works less
 * smoothly. Between the two, the safe default is the one that always scrolls.
 */
namespace HostOsProbe {

enum class HostOs
{
    Unknown, ///< nothing we can stand behind said which
    Windows,
    Linux,
    MacOs,
};

QString toString(HostOs os);
HostOs fromString(const QString& s);

/// The OS MoonlightWeb itself was built for. Used for a host that resolves to
/// this very machine, which is the one case that needs no inference at all.
constexpr HostOs thisMachine()
{
#if defined(Q_OS_WIN)
    return HostOs::Windows;
#elif defined(Q_OS_MACOS)
    return HostOs::MacOs;
#elif defined(Q_OS_LINUX)
    return HostOs::Linux;
#else
    return HostOs::Unknown;
#endif
}

/// Whether this host keeps the part of a scroll amount below one notch. False
/// for Unknown: see the header note on why that is the safe way to be wrong.
constexpr bool keepsSubNotchScroll(HostOs os)
{
    return os == HostOs::Windows || os == HostOs::MacOs;
}

/// OS implied by the IP TTL of a packet the host sent us, or Unknown.
///
/// Initial TTLs are 128 on Windows and 64 on Linux and macOS, decremented once
/// per router on the way. Only the Windows side of that is usable: 64 covers
/// both Unix-likes and they need opposite handling, so it is left Unknown
/// rather than guessed. Above 128 (255-based stacks) says nothing about a
/// GameStream host either.
HostOs fromInitialTtl(int observedTtl);

/// Everything known about a host that bears on its OS, so the decision itself
/// is one pure function a test can pin.
struct OsEvidence
{
    /// This host is the machine MoonlightWeb runs on (NvComputer::isLocalMachine).
    bool isLocalMachine = false;
    /// Proven backend type — the one an admin's own control API identified.
    /// Never a guess: BackendProbe only fills this in when a backend answered.
    QString backendType;
    /// A MultiSeat control API answered on this host's address.
    bool multiSeatApiPresent = false;
    /// serverinfo state said MJOLNIR: the original NVIDIA GameStream server.
    bool isNvidiaServerSoftware = false;
    /// TTL seen on a packet from this host, 0 when none has been seen yet.
    int observedIpTtl = 0;
};

/// Work the OS out from what is known. Pure; no network.
HostOs infer(const OsEvidence& ev);

} // namespace HostOsProbe
