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

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace mw::native {
struct DisplayInfo;
}

/**
 * @brief "MoonlightWeb Virtual Display": the screen this machine streams when
 * it has none, or when the one it has is not the one to stream.
 *
 * ── What it is ──────────────────────────────────────────────────────────────
 *
 * A virtual display that belongs to MoonlightWeb, shown on the native host's
 * card as one more app, named after itself. It is OFF by default: opening
 * that card turns it on, makes it the primary display, streams it, and when
 * the last stream on it ends it is turned off again and the previous primary
 * display gets its role back. A PC with monitors keeps them; a headless PC —
 * installed through a TV, then left without a screen — has exactly this one
 * card to stream.
 *
 * ── Windows: a bundled, signed, third-party driver ─────────────────────────
 *
 * Windows has no virtual display of its own; an Indirect Display (IddCx)
 * driver is the only way, and one we write ourselves could not be signed by
 * this project's CI (Microsoft attestation needs an EV certificate). So the
 * driver is VirtualDrivers/Virtual-Display-Driver — MIT, SignPath-signed,
 * maintained — shipped INSIDE the installer (backend/installer/drivers/vdd,
 * three files, 270 KB) and pinned by SHA-256 here: the elevated helper
 * re-hashes every file before SetupAPI sees it, and trusts the catalog's
 * signer only for the duration of the install.
 *
 * The installer asks once (Skip / Accept, like the Internet page), creates a
 * root-enumerated device node for the driver with the friendly name
 * "MoonlightWeb Virtual Display", and disables it. That friendly name is what
 * makes the node OURS: a "VDD by MTT" the owner installed before keeps its
 * node, its settings and its monitor, we never touch them, and an update or a
 * reinstall finds our node and creates no second one. Nothing in the app
 * uninstalls it: only the MoonlightWeb uninstaller (or Device Manager) does.
 *
 * ── Elevation without anyone at the desk ───────────────────────────────────
 *
 * Enabling and disabling a device node needs administrator rights, and the
 * production server runs unprivileged (logon task, LeastPrivilege). A UAC
 * prompt would appear on a desktop nobody may be sitting at, so the installer
 * (which is elevated) registers a trigger-less task, RunLevel=HighestAvailable,
 * whose fixed action is OUR exe with `--vdisplay-apply`. The server writes a
 * request file holding parameters only — never a path to run — starts the
 * task, and reads the result file. The pattern of the "Update" task, with one
 * improvement: the command is the signed exe under Program Files.
 *
 * ── macOS: no driver at all ────────────────────────────────────────────────
 *
 * CoreGraphics can register a display from a process (the private
 * CGVirtualDisplay classes BetterDisplay and DeskPad are built on). The card
 * is always there; opening it creates the display in this process, makes it
 * the main display, and the display goes away with the last stream. The seam
 * is mw::native::vdisplay (native-host/include/mw/native/VirtualDisplay.h).
 *
 * ── Linux Wayland: a monitor the compositor makes ───────────────────────────
 *
 * The card is there whenever the probe lists that display (key
 * "portal-virtual"); the worker's own session makes the monitor — at the
 * stream's size, the client's screen to the pixel under "Match my screen"
 * (mw::native::sizeVirtualMonitorToClient), and at 240 Hz like the Windows
 * display (SessionConfig::virtualRefreshHz) — and it is gone when the stream
 * that made it ends — see livesInStream().
 *
 * On GNOME (42 and later) Mutter makes it, asked over its own D-Bus screen
 * cast, with no dialog: a host with no screen at all streams from the first
 * time (plan Idées Punktfunk C2). The streams on it share it, as on Windows:
 * the owner's makes it the main display, on the left of the others and none
 * switched off (SessionConfig::virtualPrimary); a guest's records that same
 * monitor, the owner's desktop; a guest with nobody else streaming makes one
 * itself, and moves to the owner's when it comes. The portal's VIRTUAL source
 * (GNOME 46+) stays behind it, for a Mutter that turns the direct route down.
 * KDE Plasma 6's portal has no such source; KWin makes the monitor itself,
 * for a binary its installed .desktop grant names (the package ships one), at
 * 60 Hz, a guest's beside the owner's. No session type but Wayland has either:
 * an X11 session (a machine with NVIDIA's driver, often) shows no card.
 *
 * ── The mode ────────────────────────────────────────────────────────────────
 *
 * 1920×1080 at 120 Hz, SDR, on every platform: 120 Hz because the capture
 * cadence is what bounds the latency, 1080p because every encoder on every
 * bench carries it at that rate.
 *
 * A client that asks for "Match my screen" gets its own size instead, made on
 * demand: the size travels with the Activate request, the driver's mode list
 * is rewritten with it first (the file is ours — see settingsXml()), and the
 * driver reads that list when the device starts, which is exactly what
 * turning the display on does. A phone held in the hand therefore streams a
 * 2532×1170 desktop that no monitor on the market has. An unknown or
 * out-of-bounds size, or a machine whose settings file belongs to someone
 * else's VDD, quietly keeps 1080p — the mode stage is never fatal.
 *
 * The rate travels the same way, and always. On Windows and Linux it is
 * 240 Hz whatever the client's (refreshForStream, kFasterThanStream, plan
 * framerate-hote of 30/09/2026): a picture the host paints waits for the
 * compositor at most 1/240 s, and the stream keeps the client's own cadence
 * — the browser measures it (util/RefreshRate.js) and /start carries it as
 * `client_refresh_mhz` — by taking the first present of each of its
 * intervals. On macOS the display runs at the stream's rate; no measurement
 * — an old client, a hidden tab at launch — keeps the 120 Hz default.
 *
 * Under `vddmatch` (Windows, on since V2 of plan glissade and the eye test of
 * 16/10/2026) the rate is a whole multiple of the client's own instead, to the
 * millihertz (matchedRefreshMilliHz): 239.744 Hz for a "120 Hz" panel that
 * runs at 119.872, where 240.000 beats against it. Every rate from here to
 * the driver is therefore in millihertz: the request file, the driver's list
 * (decimal), and the mode Windows applies (an exact fraction).
 *
 * Everything in this header that has no OS dependency (the request parser,
 * the settings XML, the names) is pure and covered by
 * tests/test_virtual_display.cpp on every platform.
 */
namespace VirtualDisplay {

// ── Names ───────────────────────────────────────────────────────────────────

/// What the display is called everywhere: the app card, the device node's
/// friendly name in Device Manager, the macOS descriptor. Per edition, so a
/// DEV install beside a PROD one has a node of its own: "MoonlightWeb Virtual
/// Display", "MoonlightWebDev Virtual Display".
QString displayNameFor(const QString& productName);
QString displayName();

/// The elevated task's name for an edition: "MoonlightWeb Virtual Display",
/// "MoonlightWebDev Virtual Display", "MoonlightWeb-dev Virtual Display" for a
/// --dev instance (which the installer never registers: --dev is a scratch
/// instance and gets the manual path).
QString taskNameFor(const QString& productName, bool devFlag);

/// taskNameFor() for this process.
QString taskName();

/// The fixed argument the task runs our exe with.
inline QString applyArgument()
{
    return QStringLiteral("--vdisplay-apply");
}

// ── The mode ────────────────────────────────────────────────────────────────

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kRefreshHz = 120;

/// What a requested mode may be. The floor is VGA's 480 lines — the smallest
/// mode Windows still offers, and one the driver's own sample lists, so a
/// client on an 800×600 desktop gets 800×600 and not a shape nobody asked
/// for. The ceiling is what the driver and every encoder on the bench carry.
/// Odd sizes are rounded down because a display mode is made of whole
/// macroblocks on the encoder that follows it. This is the last-resort guard:
/// the client pins a size to these bounds AT ITS OWN SHAPE first
/// (fitModeBounds in util/StreamResolution.js).
constexpr int kModeMin = 480;
constexpr int kModeMax = 4096;

/// What a requested refresh rate may be. The floor is a cadence a desktop is
/// still usable at, the ceiling is the product's own: the rate its display
/// runs at on Windows whatever the client's (refreshForStream), and never
/// past but for vddmatch's whole multiple of the client's (up to
/// kMatchedRateMaxHz). The driver itself goes to 500 Hz, which only the bench
/// reaches (MW_VDD_REFRESH, plan framerate-hote): a request is guarded against
/// the driver's ceiling, the product's is applied where the rate is chosen.
constexpr int kRateMin = 24;
constexpr int kRateMax = 240;
constexpr int kRateDriverMax = 500;

/// Every rate below the stream's choice of one is in millihertz: a client's
/// screen that refreshes at 119.872 Hz gets a display at 239.744, exactly —
/// the driver takes a decimal rate in its list and Windows applies the mode
/// as the rational it is (29968/125), where a whole 240 beats against that
/// screen and loses a picture every 7.8 s (plan glissade, V0 of 14/10/2026).
constexpr int kMilliHz = 1000;

/// The mode a request asks for, pinned into bounds and made even. Returns
/// false — and leaves 0×0 — when there is nothing usable to ask for, which is
/// the normal case: the default mode then applies.
bool normaliseMode(int& width, int& height);

/// The refresh rate a request asks for, in millihertz, pinned into
/// [kRateMin, kRateDriverMax] Hz. Returns false — and leaves 0 — when there
/// is none, in which case kRefreshHz applies.
bool normaliseRate(int& milliHz);

/// A rate in millihertz as the driver's settings file spells it: "240" when
/// it is whole, "239.744" when it is not (no trailing zeros).
QString rateText(int milliHz);

/// A rate in millihertz as the display mode Windows applies: the fraction
/// milliHz / 1000 in lowest terms (29968/125 for 239.744, 240/1 for 240).
/// The driver lists exactly that mode, and SetDisplayConfig finds no other —
/// a 240/1 asked of a display that lists 239.744 is refused, silently.
struct RateFraction
{
    unsigned numerator = 0;
    unsigned denominator = 1;
};
RateFraction rateFraction(int milliHz);

/// The display's rate matched to the client's screen (key `vddmatch`, plan
/// glissade, section V): k × @p clientMilliHz for the largest whole k that
/// keeps it within @p capHz, so that the stream's gate locks on a whole
/// number of the display's presents AND each of them lands on a refresh of
/// the client's screen. Only when the stream (@p streamFps) runs at the
/// client's rate or a whole divisor of it, to 0.2 % (the gate's own
/// tolerance, PresentLattice.h): a 120 fps stream for a 119.872 Hz screen,
/// a 60 for a 59.94. And only for a k × that divisor of 1 or an even number:
/// "Auto"'s detection may step the stream to twice its rate (frontend
/// CadenceStepper.js), which must still divide the display.
///
///   - 119.872 Hz, 120 fps, cap 330 → 239.744; 59.94 Hz, 60 fps → 239.76
///   - 144 Hz, 144 fps → 288; 165 Hz, 165 fps → 330
///   - 240 Hz, 240 fps → 240 (k = 1)
///
/// 0 when nothing fits — no measurement, a stream that is no divisor of the
/// client's rate (a 120 fps stream for a 144 Hz screen), a screen faster than
/// the cap: the caller keeps the display at its usual rate.
int matchedRefreshMilliHz(int clientMilliHz, int streamFps, int capHz);

/// The ceiling matchedRefreshMilliHz() is given by the product: 330 Hz, a
/// 165 Hz screen's twice, which DualRTX's RTX held with a wide margin (V2 of
/// plan glissade, 15/10/2026; a host that does not gets less,
/// matchCapAfterRefusals). The bench raises it with `vddmatchmax=`, up to the
/// driver's 500.
constexpr int kMatchedRateMaxHz = 330;

/// The ceiling matchedRefreshMilliHz() is given on a host whose capture did
/// not hold a display rate (the native engine's CaptureHold, kept by
/// AppSettings::vddMatchRefused as {gpu, milli_hz, version}): 2 % under the
/// lowest one refused for this @p version on a GPU of @p gpus — all of them
/// when @p gpus is empty, nothing known yet of the machine's — so that a
/// 165 Hz screen refused 330 gets 165 (×1), and a 144 Hz one on the same host
/// still tries 288. @p capHz when nothing applies. An update tries again.
///
/// A refusal is no verdict for good (D27, option B of 28/10/2026): two
/// captures that missed by a hair under a passing load had held a bench's
/// display at the client's rate for the whole version. Each one keeps the
/// cap for refusalHoldMs() of its `count` from its `at` (ms since the epoch),
/// then the next stream tries the rate again. An entry without `at` — written
/// before the hold expired — or one dated after @p nowMs is no longer in force.
int matchCapAfterRefusals(int capHz, const QJsonArray& refused, const QString& version,
                          const QStringList& gpus, qint64 nowMs);

/// How long the @p count-th refusal in a row of one GPU and rate keeps the
/// cap: 12 h for the first, twice as long for each one after, at most 14
/// days. A host whose capture really cannot hold the rate gets one stream of
/// folded frames per hold — after 12 h, 1, 2, 4, 8 days, then every 14.
qint64 refusalHoldMs(int count);

/// What EncoderTuning::vddMatch's -1, the product's, means: on under Windows
/// since V2 of plan glissade (15/10/2026: no picture lost to the beat, an age
/// without its saw-tooth, the capture held up to 330 Hz) and a blind eye test
/// (16/10, preferred twice out of two). Off elsewhere: Linux's portal
/// monitor takes the stream's rule (refreshForStream), macOS is unmeasured.
#if defined(Q_OS_WIN)
constexpr bool kVddMatchProduct = true;
#else
constexpr bool kVddMatchProduct = false;
#endif

/// The refresh rate a stream asks of the display.
///   - @p faster (the product on Windows, decision A of plan framerate-hote,
///     30/09/2026): kRateMax, whatever the stream's rate. The stream keeps
///     its own cadence; a picture the host renders waits for the compositor
///     at most 1/240 s instead of up to a whole frame of the stream.
///   - Otherwise its own @p streamFps, under kRateMax; 0 leaves the display's
///     default rate.
/// The bench's MW_VDD_REFRESH (@p benchHz > 0) wins either way, up to the
/// driver's own ceiling.
int refreshForStream(int streamFps, int benchHz, bool faster = false);

/// The size a stream asks of the display, 0 x 0 for the mode it has.
///   - @p match ("Match my screen"): the client's own size, whatever it is.
///   - Otherwise the stream's size when it holds more pixels than the default
///     mode (kWidth x kHeight): a 1440p or 2160p stream gets a desktop of its
///     size, instead of the 1080p one scaled up or, under a box the host
///     never upscales, a stream that stays at 1080p without a word.
///   - A smaller stream keeps the display's mode (the default 1080p desktop,
///     scaled down), as before.
/// The size still goes through normaliseMode where it is applied.
struct ModeSize
{
    int width = 0;
    int height = 0;
};
ModeSize sizeForStream(int width, int height, bool match);

/// Whether this platform's virtual display runs faster than the stream
/// (refreshForStream's @p faster): Windows (decision A of plan framerate-hote,
/// 30/09/2026) and Linux, whose portal monitor the stream itself makes (C0 of
/// plan Idées Punktfunk, 01/10/2026). macOS keeps the stream's own rate —
/// unmeasured there.
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
constexpr bool kFasterThanStream = true;
#else
constexpr bool kFasterThanStream = false;
#endif

// ── The bundled driver (Windows x64) ───────────────────────────────────────
//
// VirtualDrivers/Virtual-Display-Driver release 25.7.23, the bare driver
// package (inf + cat + dll), as committed under backend/installer/drivers/vdd
// and installed to {app}\drivers\vdd. Bumping the version means recomputing
// every hash below from the published file; a file that does not match is
// never handed to SetupAPI.

/// One file of the package as it must be found next to the exe.
struct DriverFile
{
    const char* name;   ///< file name under bundledDriverDir()
    const char* sha256; ///< lowercase hex
};

/// The three files SetupAPI consumes.
inline const std::array<DriverFile, 3>& driverFiles()
{
    static const std::array<DriverFile, 3> files = {{
        {"MttVDD.inf", "550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d"},
        {"mttvdd.cat", "08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6"},
        {"MttVDD.dll", "c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05"},
    }};
    return files;
}

/// The root-enumerated hardware id the INF binds to.
inline QString hardwareId()
{
    return QStringLiteral("Root\\MttVDD");
}

/// SHA-1 thumbprint of the certificate that signed the catalog (SignPath
/// Foundation, issued by GlobalSign, valid to 2027-09-07). A user-mode driver
/// package installs silently only when its publisher is trusted: the elevated
/// helper adds THIS certificate — found inside the catalog, and only if its
/// thumbprint matches — to the machine's Trusted Publishers for the duration of
/// the install, then removes it again. Nothing signed by anyone else is ever
/// trusted, and the trust does not outlive the operation.
inline QString catalogSignerThumbprint()
{
    return QStringLiteral("3cf8cf26d8ba266c3a483ab7d26d4a818e317d76");
}

/// Where the driver reads its configuration (fixed by the driver, not by us).
/// Shared by every instance of the driver on the machine — which is why a
/// file already there (the owner's own VDD) is left exactly as it is.
inline QString settingsXmlPath()
{
    return QStringLiteral("C:/VirtualDisplayDriver/vdd_settings.xml");
}

/// The driver files as the installer laid them out: {app}\drivers\vdd. Derived
/// from the exe's own location, never from a request.
QString bundledDriverDir();

// ── Request / result files ──────────────────────────────────────────────────

struct Request
{
    enum class Action
    {
        Install,    ///< installer: create our node, disabled (no-op if there)
        Uninstall,  ///< uninstaller: remove our node, the package if unused
        Activate,   ///< a stream starts: enable, set the mode, make primary
        Deactivate, ///< the last stream ended: restore the primary, disable
        OthersBack, ///< the owner left, guests stay: the other displays back on
    };
    Action action = Action::Activate;
    /// Deactivate: the display to make primary again, as Activate reported
    /// it (Result::previousPrimary). Empty: leave the choice to the OS.
    QString restorePrimary;
    /// Activate: the size the client asked the display to have ("Match my
    /// screen"). 0×0 — the usual case — is the default mode. Always within
    /// [kModeMin, kModeMax] and even once parsed.
    int width = 0;
    int height = 0;
    /// Activate: the rate the display is to run at, in millihertz
    /// (refreshForStream, or matchedRefreshMilliHz). 0 — none asked — is
    /// kRefreshHz. Always within [kRateMin, kRateDriverMax] Hz once parsed.
    /// The request file says it as `refresh_mhz`; a file with the whole
    /// hertz of an older server (`refresh`) is read too.
    int refreshMilliHz = 0;
    /// Activate, macOS: make the display able to show HDR (EDR) — asked only
    /// when the viewer asked for HDR. Applied in process, never written to
    /// the request file: Windows' display is SDR, whatever this says.
    bool hdr = false;
    /// Activate, Windows, bench only: leave the display in HDR (advanced
    /// colour) instead of SDR, so a stream that asked for HDR gets it. Set by
    /// the server only when the viewer asked for HDR AND MW_BENCH_VD_HDR=1 is
    /// in its environment (the Auto-bitrate bench measures HDR streams this
    /// way); written as `bench_hdr`. Off, the display is SDR as always.
    bool benchHdr = false;
    /// Activate, Windows, bench only: the primary display stays the primary.
    /// Ours stays where the activation put it (on the right of the others
    /// when the layout from before is imposed) and is not made the primary
    /// (LayoutStep::Primary is skipped).
    /// The stream still captures ours: the Virtual Display card names it,
    /// not the primary. Set by the server only when MW_BENCH_VD_SECONDARY=1
    /// is in its environment (a bench on a desk whose primary must not move);
    /// written as `bench_secondary`. An exclusive stream turns the others off
    /// all the same, which leaves ours the only display. Off, ours is the
    /// primary while it streams, as always.
    bool benchSecondary = false;
    /// Activate, Windows: every other display goes off while ours streams
    /// (issue #39), the owner's choice (AppSettings::virtualDisplayExclusive),
    /// for the owner's own streams only: see othersGoOff(). Never saved to
    /// Windows' database: see savesLayout().
    bool exclusive = false;
    /// Activate, Windows: the GPU the display is to be drawn by, as the OS
    /// names it (AppSettings::virtualDisplayGpu, the owner's choice on the
    /// display's card). Empty — the default — leaves it to the driver. The
    /// driver goes by the name alone, so of two GPUs that share one it takes
    /// whichever it meets first. Only ever written into a settings file of
    /// ours (settingsXml), never into an owner's own. Always a validGpuName()
    /// once parsed.
    QString gpu;
};

/// Parse and validate a request. The file is read by an elevated process, so
/// its contents are a choice among five verbs, one display key and a mode,
/// never free-form input. Returns nullopt with @p error filled on any deviation.
std::optional<Request> parseRequest(const QByteArray& json, QString* error);
QByteArray toJson(const Request& req);
QString toString(Request::Action action);

/// The helper's error when the display never came up after the node was
/// switched on — the driver read a mode list it could not bring up (a
/// client's 1784×1160 at 240 Hz beside an owner's own modes, 11/10/2026).
QString notAppearedError();

/// What to try next when activating @p tried failed with @p error: the same
/// request one step closer to the default mode — the client's size dropped
/// first (a rate the client's screen runs at is worth more than its shape),
/// then its rate. nullopt when there is nothing left to drop, or when the
/// failure is not one a mode explains: a launch at the default mode is
/// better than no launch, which is what normaliseMode() already promises.
std::optional<Request> fallbackTowardsDefault(const Request& tried, const QString& error);

/// Who a stream's activation of the display is for.
enum class Asker
{
    Owner, ///< a signed-in session's /start
    Guest, ///< someone with an invitation, who may be anywhere
};

/// Whether an activation has every other display off (Request::exclusive): the
/// option in force (@p optionOn), and the owner asking. A guest may open an
/// invitation cold while someone works at the PC, and an invitation gives no
/// say over its screens: the guest's stream runs with them on. An owner who
/// finds a guest on the display (@p guestOn) leaves them as they are, on until
/// the owner's next stream; off already (@p othersOff), they stay off through
/// the owner's change of mode.
bool othersGoOff(Asker asker, bool optionOn, bool guestOn, bool othersOff);

/// The option an owner's stream brings to othersGoOff(): two keys, both
/// needed. The admin allows it (@p allowed, AppSettings::virtualDisplayExclusive)
/// and the viewer turned it on in this browser's streaming settings (@p choice,
/// sent with the stream). A browser that never chose (@p hasChoice false) takes
/// what the PC did before the choice moved to the viewer (@p inherited): on
/// where the admin's box was already ticked, so an update changes nothing.
bool ownerWantsOthersOff(bool allowed, bool hasChoice, bool choice, bool inherited);

/// The display cards drawn dark on the native host's card: with the other
/// displays off (@p othersOff), the ones last seen lit (@p lit, in their
/// order) that the PC no longer lists (@p listed). Windows detaches them, so
/// without this their cards would vanish for the length of the stream. None
/// with the displays on.
QList<int> darkDisplays(const QList<int>& lit, const QList<int>& listed, bool othersOff);

/// With a stream still on the display: whether the other displays come back on
/// now. They were the owner's to turn off, so they go with the owner's stream
/// (@p ownerOn), whatever the guests still watching.
bool othersComeBack(bool ownerOn, bool othersOff);

/// The display layout changes the Windows helper makes, in the order an
/// activation takes them (Restore is a deactivation's).
enum class LayoutStep
{
    Impose,    ///< the layout from before, every display as it was, plus ours
    Prune,     ///< what Windows lit up beside ours, off again
    Mode,      ///< ours at the client's size and rate
    Primary,   ///< ours the primary
    Exclusive, ///< every display but ours off (Request::exclusive)
    Restore,   ///< the layout from before, ours off
};

/// Whether @p step goes to Windows' database, the layout Windows replays the
/// next time this set of displays is attached — after a reboot, an unplug, or
/// the node going off with no server left to restore anything. @p othersOff:
/// an exclusive stream has the other displays off right now. A layout with
/// them off is never saved: kept out of the database, they come back on
/// whatever ends the stream, a crash included. Impose turns them back on in
/// the same change, so what it saves has them on.
bool savesLayout(LayoutStep step, bool othersOff);

/// Whether the other displays are off after @p step, given @p othersOff
/// before it and whether it went through (@p ok).
bool othersOffAfter(LayoutStep step, bool othersOff, bool ok);

/// The guard an exclusive stream leaves beside the server
/// (VirtualDisplayApply::guard): what it does on each look.
enum class GuardStep
{
    Wait,    ///< the server runs and the other displays are off
    Leave,   ///< they are not off, or that cannot be read: nothing to touch
    Restore, ///< the server is gone with them off: bring them back
};
GuardStep guardStep(bool serverAlive, bool othersOff);

inline QString guardArgument()
{
    return QStringLiteral("--vdisplay-guard");
}

struct Result
{
    bool ok = false;
    QString stage; ///< where it ended: "snapshot" | "driver" | "mode" | "done"
    QString error; ///< English, for the log and the dialog
    bool rebootRequired = false;
    QString display; ///< GDI name of the display that came up ("\\.\DISPLAY3")
    /// Activate: the display that was primary before ours took the role — the
    /// monitor device path on Windows, the CG display id on macOS. Handed
    /// back on Deactivate as Request::restorePrimary.
    QString previousPrimary;
};

/// Tolerant of an empty or partial file: the server polls while the helper is
/// still writing. Returns nullopt until a complete object is there.
std::optional<Result> parseResult(const QByteArray& json);
QByteArray toJson(const Result& res);

/// The result file at @p path, read and let go of before anyone acts on it:
/// Windows deletes no file that is still open, and acting on a verdict means
/// deleting it. Held open, the fallback's next run read its predecessor's
/// failure again (DualRTX, 13/10/2026: 1108×720, then 0×0 "did not come up"
/// a quarter of a second after it was asked, while it came up — and /start
/// answered 500). nullopt while there is no complete object to read.
std::optional<Result> readResult(const QString& path);

/// The driver's vdd_settings.xml: a single monitor, our mode first (the
/// driver picks the first as default), the common sizes after it, SDR. A
/// @p width × @p height of 0 is the default 1920×1080; anything else is the
/// client's own size, added ahead of the list. @p refreshMilliHz of 0 is
/// kRefreshHz; anything else is the rate the display is to run at, which
/// every mode in the file is then offered at.
///
/// Written when none exists, and rewritten — with the node restarted so the
/// driver re-reads it — when the mode changes. Never over a file this project
/// did not write: an owner's own VDD configured that one (see isOurSettings).
///
/// @p gpu is the GPU to draw it (Request::gpu), empty for the driver's own
/// choice; a bench's MW_VDD_GPU still comes first.
QString settingsXml(int width = 0, int height = 0, int refreshMilliHz = 0,
                    const QString& gpu = QString());

/// The GPU a vdd_settings.xml names (<gpu><friendlyname>), ours or an owner's:
/// empty when it names none, or "default" — the driver's own choice.
QString gpuOfSettings(const QString& xml);

/// Whether @p name may name a GPU in a request and in the settings file:
/// non-empty, at most 128 characters, and nothing the XML or an elevated
/// reader would read as anything but text (no control character, no < > & " ').
/// Inline: AppSettings checks it too, in every binary that builds the settings.
inline bool validGpuName(const QString& name)
{
    if (name.trimmed().isEmpty() || name.size() > 128) return false;
    for (const QChar c : name) {
        if (c.category() == QChar::Other_Control) return false;
        if (c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('&') ||
            c == QLatin1Char('"') || c == QLatin1Char('\''))
            return false;
    }
    return true;
}

/// Does this vdd_settings.xml carry our marker, i.e. did we write it?
bool isOurSettings(const QString& xml);

/// @p existing — a settings file someone else wrote — with @p width ×
/// @p height at @p refreshMilliHz added to its mode list, ahead of the rest, and
/// everything else left exactly as it was. The one edit this project makes to
/// a file it does not own: without the mode in that list the driver will not
/// offer it, and "Match my screen" has nothing to switch to. Returns the file
/// unchanged (@p changed false) when that very size and rate are already
/// listed, when there is no `<resolutions>` element to add them to, or when
/// there is no size to add.
QString settingsWithMode(const QString& existing, int width, int height, int refreshMilliHz,
                         bool* changed);

/// @p existing with the mode settingsWithMode() added for @p width ×
/// @p height at @p refreshMilliHz taken out again: the block exactly as it went in,
/// so the owner's file comes back byte for byte, or else the first block that
/// lists those three values. Returns the file unchanged (@p changed false)
/// when no block lists that mode.
QString settingsWithoutMode(const QString& existing, int width, int height, int refreshMilliHz,
                            bool* changed);

/// A mode this project added to another VDD's list.
struct AddedMode
{
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    bool operator==(const AddedMode& o) const
    {
        return width == o.width && height == o.height && refreshMilliHz == o.refreshMilliHz;
    }
};

/// The record of those modes, one `1784x1160@240` (or `@239.744`) a line: what the next
/// activation, or this one's end, takes out of the owner's file. Without it
/// the list grew by a mode with every new client — on DualRTX, 03/10/2026, a
/// 1280×720 at 240 Hz left by a TV's stream, after which the driver no longer
/// brought the display up for a Mac's 1784×1160 at 240. Lines that do not
/// read as a mode are skipped.
QList<AddedMode> parseAddedModes(const QString& text);
QString addedModesText(const QList<AddedMode>& modes);

/// Where that record lives: beside the driver's file, which both editions
/// write to, so either takes out what the other added.
inline QString addedModesPath()
{
    return QStringLiteral("C:/VirtualDisplayDriver/moonlightweb-added-modes.txt");
}

// ── The other displays' layout ──────────────────────────────────────────────

/// A display's desktop rectangle, as Windows places it.
struct DesktopRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// Where our display goes beside @p others, the layout from before it: on the
/// right of the one that reaches furthest right, top edges aligned, so the two
/// share an edge (the topmost of them on a tie). (0, 0) when there is none.
///
/// Windows, left to itself, brings a new set of displays up from its database,
/// and keeps there the modes each one had the last time that set was on: an
/// owner's own VDD on the same driver came back at 800×600 at 30 Hz, the first
/// mode of its list, for every stream on ours (N95, 16/10/2026). So the
/// activation applies the layout from before — every other display in its mode
/// and at its place, ours here — and saves that one as the set's.
QPoint placeBeside(const QList<DesktopRect>& others);

// ── Paths ───────────────────────────────────────────────────────────────────

/// Per-user staging directory. Not %TEMP%: it holds files an elevated process
/// will act on, and C:\Windows\Temp is writable by every local account.
QString stagingDir();
QString requestPath();
QString resultPath();
/// The helper's marker: an exclusive stream has the other displays off.
QString othersOffPath();

// ── Status ──────────────────────────────────────────────────────────────────

struct Status
{
    bool supported = false; ///< this platform can have our virtual display
    bool installed = false; ///< Windows: our device node exists; macOS: always
    bool enabled = false;   ///< Windows: the node is enabled; macOS: created
    bool active = false;    ///< our display is in the probe's list right now
    bool canManage = false; ///< an elevation path exists (see method)
    QString method;         ///< "task" | "elevated" | "inprocess" (macOS) | ""
};

/// Probe everything. Cheap except for the Task Scheduler lookup, which is
/// cached for a while — nothing there changes without an install.
Status probe();

/// Serialise for the REST API and the host card.
QJsonObject toJson(const Status& st);

/// Is this display, as the engine enumerated it, OUR virtual display? Windows:
/// the monitor hanging off our device node; macOS: the display this process
/// created. Never true for another virtual display (the owner's own VDD, a
/// dummy plug), which stays an ordinary "Display N".
bool isOurs(const mw::native::DisplayInfo& display);

/// Windows: does our device node exist right now? (disabled counts)
bool nodePresent();

/// Windows: is our device node enabled?
bool nodeEnabled();

/// Windows: is the process token elevated?
bool processElevated();

/// Windows: is a run of the elevated task still going? Its helper writes its
/// verdict and only then exits, and the task ignores a start while a run is
/// going (MultipleInstancesPolicy IgnoreNew): a start in that gap is lost,
/// with schtasks reporting success all the same. False elsewhere, and when
/// the Task Scheduler cannot be asked.
bool elevatedTaskRunning();

// ── The in-process display (macOS) ─────────────────────────────────────────
//
// On macOS the display is not a driver but an object this process holds
// (mw::native::vdisplay, CoreGraphics' virtual display): no elevation, no
// helper — Status::method is "inprocess" and the job applies the request
// right here.

/// Activate (create + make main) or Deactivate (restore main + release).
/// Synchronous; the display may still be coming online when this returns
/// (the job polls).
bool applyInProcess(const Request& req, Result* result);

/// At startup: put the display back in its off state if the previous process
/// left it on (killed mid-stream). Windows: disable the node and restore the
/// remembered primary. macOS: nothing to do — the display died with the
/// process — beyond forgetting the record.
void resetAtStartup();

/// Linux: the display is made by the stream's own ScreenCast session (the
/// portal's VIRTUAL source, at the client's size) and goes with it. Nothing
/// for the server to turn on before a launch or off after the last stream —
/// the job is skipped altogether. False on Windows and macOS.
bool livesInStream();

} // namespace VirtualDisplay
