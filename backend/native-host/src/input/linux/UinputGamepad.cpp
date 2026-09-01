/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "UinputGamepad.h"

#include "../../core/Log.h"
#include "../Ds4Mapping.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mw::native::input {
namespace {

constexpr const char* kUinputPath = "/dev/uinput";

/// How many effects a game may have loaded at once. Sixteen is what real pads
/// report and what SDL expects to find; it is also the cap on what one client
/// can make us remember (§13).
constexpr uint32_t kMaxEffects = 16;

/// Sticks keep the protocol's own range, so no conversion happens at all: what
/// the browser measured is what the game reads.
constexpr int32_t kStickMin = -32768;
constexpr int32_t kStickMax = 32767;
/// Triggers are 0..255, which is the range the protocol carries them in.
constexpr int32_t kTriggerMax = 255;

/// The identity a profile presents. On evdev this IS the profile: the buttons
/// and axes below are identical either way, and SDL picks its prompts from the
/// name and the ids.
struct Identity
{
    const char* name;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

Identity identityFor(UinputGamepad::Profile profile)
{
    if (profile == UinputGamepad::Profile::DualShock4) {
        // Sony's own ids for the second-generation DualShock 4 (CUH-ZCT2). SDL
        // and the kernel's hid-sony both know this pair, which is the point —
        // an unknown pair would be treated as a generic pad and lose the
        // PlayStation button prompts the profile exists to provide.
        return {"Sony Interactive Entertainment Wireless Controller", 0x054C, 0x09CC, 0x8111};
    }
    // The wired Xbox 360 pad: the most widely recognised gamepad identity on
    // Linux, and the one every SDL mapping database has had for a decade.
    return {"Microsoft X-Box 360 pad", 0x045E, 0x028E, 0x0110};
}

/// The buttons the device declares. Both profiles expose the same set: on
/// evdev, BTN_SOUTH is the bottom face button whatever is printed on it.
constexpr uint16_t kButtons[] = {
    BTN_SOUTH,  BTN_EAST,  BTN_NORTH, BTN_WEST,  BTN_TL,     BTN_TR,
    BTN_SELECT, BTN_START, BTN_MODE,  BTN_THUMBL, BTN_THUMBR,
};

/// The axes, with the range each is declared over.
struct AxisSpec
{
    uint16_t code;
    int32_t minimum;
    int32_t maximum;
};

constexpr AxisSpec kAxes[] = {
    {ABS_X, kStickMin, kStickMax},   {ABS_Y, kStickMin, kStickMax},
    {ABS_RX, kStickMin, kStickMax},  {ABS_RY, kStickMin, kStickMax},
    {ABS_Z, 0, kTriggerMax},         {ABS_RZ, 0, kTriggerMax},
    // The D-pad is a hat: two axes that only ever read −1, 0 or 1. Declaring it
    // as four buttons instead is what makes a pad whose D-pad "does nothing" in
    // half the games — SDL's mappings expect the hat.
    {ABS_HAT0X, -1, 1},              {ABS_HAT0Y, -1, 1},
};

std::string describeErrno(int err)
{
    switch (err) {
    case ENOENT:
        return "/dev/uinput is missing — the uinput kernel module is not loaded";
    case EACCES:
    case EPERM:
        return "/dev/uinput refused access — the udev rule granting this user is not installed";
    case ENODEV: return "/dev/uinput exists but no uinput device is available";
    default: return std::string("/dev/uinput could not be opened: ") + std::strerror(err);
    }
}

/// Write a batch of events and terminate it with the synchronisation marker.
///
/// One write() for the whole report, not one per event: the kernel takes a
/// batch, and a report split across syscalls is a report a game can observe
/// half-applied — a stick that moved on one axis and not yet the other.
bool writeReport(int fd, std::vector<input_event>& events)
{
    input_event sync{};
    sync.type = EV_SYN;
    sync.code = SYN_REPORT;
    sync.value = 0;
    events.push_back(sync);

    const size_t bytes = events.size() * sizeof(input_event);
    const ssize_t written = ::write(fd, events.data(), bytes);
    return written == static_cast<ssize_t>(bytes);
}

void addEvent(std::vector<input_event>& events, uint16_t type, uint16_t code, int32_t value)
{
    input_event event{};
    event.type = type;
    event.code = code;
    event.value = value;
    events.push_back(event);
}

} // namespace

UinputGamepad::Profile UinputGamepad::profileFor(uint8_t controllerType)
{
    if (controllerType == InputEvent::kControllerPlayStation) return Profile::DualShock4;
    return Profile::X360;
}

bool UinputGamepad::devicePresent(std::string& error)
{
    const int fd = ::open(kUinputPath, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error = describeErrno(errno);
        return false;
    }
    ::close(fd);
    error.clear();
    return true;
}

UinputGamepad::UinputGamepad(RumbleSink onRumble)
    : m_OnRumble(std::move(onRumble))
{}

UinputGamepad::~UinputGamepad()
{
    stop();
}

bool UinputGamepad::start(std::string& error)
{
    if (m_Started) return true;
    // Nothing is created here — the pads appear as the client announces them.
    // This only answers "could we", so a session with no gamepad never opens a
    // device it will not use.
    if (!devicePresent(error)) return false;

    m_WakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_WakeFd < 0) {
        error = std::string("could not create the feedback wake-up descriptor: ") +
                std::strerror(errno);
        return false;
    }

    m_Stopping.store(false);
    m_Feedback = std::thread([this]() { feedbackLoop(); });
    m_Started = true;
    return true;
}

void UinputGamepad::stop()
{
    if (!m_Started) return;

    m_Stopping.store(true);
    if (m_WakeFd >= 0) {
        const uint64_t one = 1;
        // Best-effort: the loop also wakes on its own timeout, so a failed
        // write costs a few milliseconds and nothing else.
        (void)::write(m_WakeFd, &one, sizeof(one));
    }
    if (m_Feedback.joinable()) m_Feedback.join();

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (int slot = 0; slot < kMaxPads; ++slot) destroyPad(slot);
    }

    if (m_WakeFd >= 0) {
        ::close(m_WakeFd);
        m_WakeFd = -1;
    }
    m_Started = false;
}

int UinputGamepad::connectedCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    int count = 0;
    for (const Pad& pad : m_Pads)
        if (pad.fd >= 0) ++count;
    return count;
}

UinputGamepad::Pad* UinputGamepad::ensurePad(int slot, Profile profile)
{
    if (!m_Started || slot < 0 || slot >= kMaxPads) return nullptr;
    if (m_Pads[slot].fd >= 0) return &m_Pads[slot];

    // Read/write: the force-feedback traffic comes back to us on this same
    // descriptor. Non-blocking so the feedback thread's poll() is what decides
    // when to wait, rather than a read blocking a shutdown.
    const int fd = ::open(kUinputPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log::warning(std::string("[native] could not create virtual pad ") + std::to_string(slot) +
                     ": " + describeErrno(errno));
        return nullptr;
    }

    auto fail = [&](const char* what) {
        log::warning(std::string("[native] virtual pad ") + std::to_string(slot) + ": " + what +
                     " failed: " + std::strerror(errno));
        ::close(fd);
        return nullptr;
    };

    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) return fail("UI_SET_EVBIT(EV_KEY)");
    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) return fail("UI_SET_EVBIT(EV_ABS)");
    if (::ioctl(fd, UI_SET_EVBIT, EV_FF) < 0) return fail("UI_SET_EVBIT(EV_FF)");

    for (uint16_t button : kButtons)
        if (::ioctl(fd, UI_SET_KEYBIT, button) < 0) return fail("UI_SET_KEYBIT");

    // Rumble only. The other effect types (springs, dampers) belong to wheels,
    // which are out of scope by §2.3, and claiming them would make a game
    // upload effects this pad cannot produce.
    if (::ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) < 0) return fail("UI_SET_FFBIT(FF_RUMBLE)");

    for (const AxisSpec& axis : kAxes) {
        if (::ioctl(fd, UI_SET_ABSBIT, axis.code) < 0) return fail("UI_SET_ABSBIT");
        uinput_abs_setup absSetup{};
        absSetup.code = axis.code;
        absSetup.absinfo.minimum = axis.minimum;
        absSetup.absinfo.maximum = axis.maximum;
        // No kernel-side deadzone or fuzz: the browser already sends what the
        // pad reported, and a second deadzone applied here would eat travel the
        // player can feel.
        if (::ioctl(fd, UI_ABS_SETUP, &absSetup) < 0) return fail("UI_ABS_SETUP");
    }

    const Identity identity = identityFor(profile);
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = identity.vendor;
    setup.id.product = identity.product;
    setup.id.version = identity.version;
    setup.ff_effects_max = kMaxEffects;
    std::strncpy(setup.name, identity.name, UINPUT_MAX_NAME_SIZE - 1);

    if (::ioctl(fd, UI_DEV_SETUP, &setup) < 0) return fail("UI_DEV_SETUP");
    if (::ioctl(fd, UI_DEV_CREATE) < 0) return fail("UI_DEV_CREATE");

    m_Pads[slot].fd = fd;
    m_Pads[slot].profile = profile;
    m_Pads[slot].effects.clear();
    log::info("[native] gamepad " + std::to_string(slot) + " created (" + identity.name + ")");
    return &m_Pads[slot];
}

void UinputGamepad::destroyPad(int slot)
{
    if (slot < 0 || slot >= kMaxPads) return;
    Pad& pad = m_Pads[slot];
    if (pad.fd < 0) return;

    // Destroy before close: closing alone would also remove the device, but
    // only once the last reference went away, and a game still holding it open
    // would keep a pad alive that nobody drives.
    (void)::ioctl(pad.fd, UI_DEV_DESTROY);
    ::close(pad.fd);
    pad.fd = -1;
    pad.profile = Profile::X360;
    pad.effects.clear();
}

void UinputGamepad::arrive(const InputEvent& event)
{
    const int slot = event.controllerNumber;
    if (slot < 0 || slot >= kMaxPads) return;

    std::lock_guard<std::mutex> lock(m_Mutex);

    const Profile wanted = profileFor(event.controllerType);
    // A slot re-announced as a different pad: replace the device. An evdev node
    // cannot change its identity, so a player who put down an Xbox pad and
    // picked up a DualShock would otherwise keep the wrong prompts all session.
    if (m_Pads[slot].fd >= 0 && m_Pads[slot].profile != wanted) {
        log::info("[native] gamepad " + std::to_string(slot) +
                  " changed type — replacing the virtual pad");
        destroyPad(slot);
    }

    ensurePad(slot, wanted);
}

void UinputGamepad::update(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // A client that never announced its pad still gets one, for the same reason
    // as on Windows: silence would be a far worse failure than an extra pad.
    Pad* pad = ensurePad(event.controllerNumber, Profile::X360);
    if (!pad) return;

    const uint32_t flags = static_cast<uint32_t>(event.buttonFlags);
    const auto held = [flags](uint32_t bit) { return (flags & bit) ? 1 : 0; };

    std::vector<input_event> events;
    events.reserve(sizeof(kButtons) / sizeof(kButtons[0]) + sizeof(kAxes) / sizeof(kAxes[0]) + 1);

    // Face buttons by POSITION. BTN_SOUTH is the bottom button on every pad
    // evdev describes, so A maps to it whatever the pad prints there — the same
    // rule the DualShock report follows (see Ds4Mapping).
    addEvent(events, EV_KEY, BTN_SOUTH, held(pad::kA));
    addEvent(events, EV_KEY, BTN_EAST, held(pad::kB));
    addEvent(events, EV_KEY, BTN_WEST, held(pad::kX));
    addEvent(events, EV_KEY, BTN_NORTH, held(pad::kY));
    addEvent(events, EV_KEY, BTN_TL, held(pad::kLeftShoulder));
    addEvent(events, EV_KEY, BTN_TR, held(pad::kRightShoulder));
    addEvent(events, EV_KEY, BTN_SELECT, held(pad::kBack));
    addEvent(events, EV_KEY, BTN_START, held(pad::kStart));
    addEvent(events, EV_KEY, BTN_MODE, held(pad::kGuide));
    addEvent(events, EV_KEY, BTN_THUMBL, held(pad::kLeftThumb));
    addEvent(events, EV_KEY, BTN_THUMBR, held(pad::kRightThumb));

    addEvent(events, EV_ABS, ABS_X, event.leftStickX);
    // Y inverted: the protocol has up positive (XInput's convention), evdev has
    // up negative like every other pointing axis on Linux. −32768 is clamped
    // rather than negated, which would not fit back into the axis.
    addEvent(events, EV_ABS, ABS_Y,
             event.leftStickY == kStickMin ? kStickMax : -event.leftStickY);
    addEvent(events, EV_ABS, ABS_RX, event.rightStickX);
    addEvent(events, EV_ABS, ABS_RY,
             event.rightStickY == kStickMin ? kStickMax : -event.rightStickY);
    addEvent(events, EV_ABS, ABS_Z, event.leftTrigger);
    addEvent(events, EV_ABS, ABS_RZ, event.rightTrigger);

    // The hat. Opposite directions cancel, as they must: a hat axis has no way
    // to say "left and right at once".
    addEvent(events, EV_ABS, ABS_HAT0X, held(pad::kDpadRight) - held(pad::kDpadLeft));
    addEvent(events, EV_ABS, ABS_HAT0Y, held(pad::kDpadDown) - held(pad::kDpadUp));

    // The bits above 16 — paddles, touchpad click, Share — have no button
    // declared on this device and are dropped, exactly as on Windows (§6.1).

    if (!writeReport(pad->fd, events)) {
        log::warning("[native] virtual pad " + std::to_string(event.controllerNumber) +
                     " write failed: " + std::strerror(errno));
    }
}

void UinputGamepad::remove(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    const int slot = event.controllerNumber;
    if (slot < 0 || slot >= kMaxPads || m_Pads[slot].fd < 0) return;

    // Centre everything before the device goes away, so a game is not left
    // holding a trigger that nothing can release any more. Best-effort — the
    // destroy below is what matters.
    // Zero is the resting value of every axis declared here: the sticks are
    // centred at 0, and the triggers and the hat rest there too.
    std::vector<input_event> neutral;
    for (uint16_t button : kButtons) addEvent(neutral, EV_KEY, button, 0);
    for (const AxisSpec& axis : kAxes) addEvent(neutral, EV_ABS, axis.code, 0);
    (void)writeReport(m_Pads[slot].fd, neutral);

    destroyPad(slot);
    log::info("[native] gamepad " + std::to_string(slot) + " removed");
}

void UinputGamepad::handleFeedback(Pad& pad, uint16_t type, uint16_t code, int32_t value,
                                   std::vector<RumbleEvent>& pending)
{
    if (type == EV_UINPUT && code == UI_FF_UPLOAD) {
        uinput_ff_upload upload{};
        upload.request_id = static_cast<uint32_t>(value);
        if (::ioctl(pad.fd, UI_BEGIN_FF_UPLOAD, &upload) < 0) return;

        // Only rumble was declared, so only rumble should arrive. Anything else
        // is refused rather than silently accepted and never played — a game
        // told "yes" would wait for an effect that never happens.
        if (upload.effect.type == FF_RUMBLE) {
            RumbleEvent stored;
            stored.lowFrequencyMotor = upload.effect.u.rumble.strong_magnitude;
            stored.highFrequencyMotor = upload.effect.u.rumble.weak_magnitude;
            // Bounded by the ff_effects_max the device declared, so a client
            // cannot grow this map without limit (§13).
            if (pad.effects.size() < kMaxEffects || pad.effects.count(upload.effect.id))
                pad.effects[upload.effect.id] = stored;
            upload.retval = 0;
        } else {
            upload.retval = -EINVAL;
        }
        (void)::ioctl(pad.fd, UI_END_FF_UPLOAD, &upload);
        return;
    }

    if (type == EV_UINPUT && code == UI_FF_ERASE) {
        uinput_ff_erase erase{};
        erase.request_id = static_cast<uint32_t>(value);
        if (::ioctl(pad.fd, UI_BEGIN_FF_ERASE, &erase) < 0) return;
        pad.effects.erase(static_cast<int16_t>(erase.effect_id));
        erase.retval = 0;
        (void)::ioctl(pad.fd, UI_END_FF_ERASE, &erase);
        return;
    }

    if (type != EV_FF || !m_OnRumble) return;

    // Playing an effect: `code` is the id the kernel assigned at upload, and a
    // zero value means stop. The magnitudes are already the 16-bit ones the
    // protocol carries, so unlike XUSB there is nothing to rescale here.
    RumbleEvent out;
    for (int slot = 0; slot < kMaxPads; ++slot) {
        if (&m_Pads[slot] == &pad) {
            out.controllerNumber = static_cast<uint8_t>(slot);
            break;
        }
    }
    if (value != 0) {
        const auto it = pad.effects.find(static_cast<int16_t>(code));
        if (it == pad.effects.end()) return;
        out.lowFrequencyMotor = it->second.lowFrequencyMotor;
        out.highFrequencyMotor = it->second.highFrequencyMotor;
    }
    // Queued, not called: the sink goes off to a relay and a DataChannel, and
    // handing it our own lock is how a rumble ends up deadlocked against the
    // session thread creating a pad.
    pending.push_back(out);
}

void UinputGamepad::feedbackLoop()
{
    // Timeout rather than an indefinite wait: the wake-up fd is best-effort, and
    // a shutdown must not depend on a write that could fail.
    constexpr int kPollTimeoutMs = 200;

    while (!m_Stopping.load()) {
        std::vector<pollfd> fds;
        std::vector<int> slots;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (int slot = 0; slot < kMaxPads; ++slot) {
                if (m_Pads[slot].fd < 0) continue;
                fds.push_back(pollfd{m_Pads[slot].fd, POLLIN, 0});
                slots.push_back(slot);
            }
        }
        if (m_WakeFd >= 0) fds.push_back(pollfd{m_WakeFd, POLLIN, 0});

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) continue;
            log::warning(std::string("[native] gamepad feedback poll failed: ") +
                         std::strerror(errno));
            return;
        }
        if (ready == 0) continue;

        // Drain the wake-up so it does not fire forever.
        if (m_WakeFd >= 0 && (fds.back().revents & POLLIN)) {
            uint64_t drained = 0;
            (void)::read(m_WakeFd, &drained, sizeof(drained));
            if (m_Stopping.load()) return;
        }

        std::vector<RumbleEvent> pending;
        {
            // Under the lock for the whole read: the slot could otherwise be
            // destroyed by the session thread between the poll and the ioctl,
            // and the fd reused by something else entirely.
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (size_t i = 0; i < slots.size(); ++i) {
                if (!(fds[i].revents & POLLIN)) continue;

                Pad& pad = m_Pads[slots[i]];
                if (pad.fd < 0) continue;

                input_event events[16];
                const ssize_t got = ::read(pad.fd, events, sizeof(events));
                if (got <= 0) continue;

                const size_t count = static_cast<size_t>(got) / sizeof(input_event);
                for (size_t e = 0; e < count; ++e)
                    handleFeedback(pad, events[e].type, events[e].code, events[e].value, pending);
            }
        }

        // Outside the lock, always.
        for (const RumbleEvent& rumble : pending) m_OnRumble(rumble);
    }
}

} // namespace mw::native::input
