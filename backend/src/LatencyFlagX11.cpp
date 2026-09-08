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

/**
 * Click-to-photon flag, Linux backend. The contract, the geometry and the
 * colours are LatencyFlag.h's; the two mechanisms are chosen apart from each
 * other, and deliberately so.
 *
 * ── Seeing the click: evdev, not X ──────────────────────────────────────────
 *
 * Every host on Linux injects through uinput — ours (UinputInput), Sunshine's,
 * Wolf's inside its container. A uinput device is a kernel input device like
 * any other, with one property no real mouse has: it hangs off
 * /sys/devices/virtual/. Reading button presses from the virtual pointers only
 * is therefore the exact equivalent of Windows' LLMHF_INJECTED — a physical
 * click on the host during a run comes from a different device node and is
 * never seen.
 *
 * This is why the click source is evdev rather than XInput2: it needs no X
 * server, no extension headers and no guessing about which device is a
 * "virtual" one by its name. It also keeps working under Wayland — where the
 * *overlay* cannot be shown, but where the click half of the mechanism is still
 * exactly right, should a future compositor ever offer a way to draw on top.
 *
 * The devices come and go: a host creates its virtual pointer when a session
 * starts and destroys it at the end, long after the flag was armed. The watcher
 * therefore rescans /dev/input on a timer rather than enumerating once.
 *
 * /dev/input/event* is root:input 0660. A user who is not in the `input` group
 * gets EACCES on every node, which would read as "the host injects nothing" —
 * hence the one-line diagnosis in the log and in unsupportedReason().
 *
 * ── Showing the flag: three override-redirect windows per monitor ───────────
 *
 * One window per band rather than one window painted in three: an
 * override-redirect window with a background pixel is filled by the X server
 * when it is mapped, so showing the flag is three XMapRaised calls and no
 * drawing at all. There is no Expose to wait for, and therefore no window in
 * which the capture could photograph a flag that is up but not yet painted.
 *
 * Override-redirect keeps the window manager out entirely: no decoration, no
 * focus stealing, no placement policy. XShape gives it an empty *input* region
 * so it never takes a click meant for the application underneath.
 *
 * ── Wayland ─────────────────────────────────────────────────────────────────
 *
 * Unsupported, and not by omission. No Wayland client may place a surface above
 * everything else (GNOME implements no layer-shell protocol), so the flag would
 * be drawn wherever the compositor felt like stacking it — sometimes in the
 * picture, sometimes not, which is worse than not measuring at all. The bench
 * runs its Linux click-to-photon chapter in an X11 session; everything else
 * about the Linux host is unaffected and keeps running under Wayland.
 */

#include "LatencyFlag.h"

#include <QDebug>
#include <QString>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef MW_HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#ifdef MW_HAVE_XSHAPE
#include <X11/extensions/shape.h>
#endif

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// How often /dev/input is re-read for pointers that appeared or went away.
constexpr int kRescanMs = 2000;

std::mutex g_Mutex;
std::atomic<bool> g_Running{false};
std::thread g_Thread;
int g_StopPipe[2] = {-1, -1};

/// Set once when the watcher finds it cannot read a single input node — the one
/// failure an operator can fix, and the one that otherwise looks like silence.
std::atomic<bool> g_NoInputAccess{false};

// ── Environment: which session are we in? ───────────────────────────────────

bool envSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v != '\0';
}

bool isWaylandSession()
{
    if (envSet("WAYLAND_DISPLAY")) return true;
    const char* type = std::getenv("XDG_SESSION_TYPE");
    return type && std::strcmp(type, "wayland") == 0;
}

// ── The click source: virtual pointers under /dev/input ─────────────────────

struct InputDevice
{
    int fd = -1;
    std::string node; // "event7"
    std::string name; // EVIOCGNAME
};

bool testBit(const unsigned char* bits, int bit)
{
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

/**
 * True for a device the kernel created out of thin air — uinput — as opposed to
 * one hanging off a real bus. /sys/class/input/eventN is a symlink into the
 * device tree, and only virtual devices resolve under /sys/devices/virtual/.
 * This is the whole "injected or not" test, and it costs one readlink.
 */
bool isVirtualDevice(const std::string& node)
{
    const std::string link = "/sys/class/input/" + node;
    char resolved[PATH_MAX];
    const ssize_t n = ::readlink(link.c_str(), resolved, sizeof(resolved) - 1);
    if (n <= 0) return false;
    resolved[n] = '\0';
    return std::strstr(resolved, "/devices/virtual/") != nullptr;
}

/// Open a virtual pointer that can report a left button, or return fd -1.
int openIfVirtualPointer(const std::string& node, std::string& nameOut, bool& deniedOut)
{
    if (!isVirtualDevice(node)) return -1;

    const std::string path = "/dev/input/" + node;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        // Only EACCES is worth reporting: ENODEV and friends are a device that
        // vanished between readdir and open, which happens all the time.
        if (errno == EACCES) deniedOut = true;
        return -1;
    }

    unsigned char keys[(KEY_MAX / 8) + 1] = {};
    if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !testBit(keys, BTN_LEFT)) {
        ::close(fd);
        return -1;
    }

    char name[256] = {};
    if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) std::strcpy(name, "(unnamed)");
    nameOut = name;
    return fd;
}

void closeDevices(std::vector<InputDevice>& devices)
{
    for (InputDevice& d : devices)
        if (d.fd >= 0) ::close(d.fd);
    devices.clear();
}

/**
 * Re-read /dev/input, keeping the devices already open and adopting the ones
 * that appeared. Devices that went away are dropped when their read fails, so
 * this only ever adds.
 */
void rescanDevices(std::vector<InputDevice>& devices)
{
    DIR* dir = ::opendir("/dev/input");
    if (!dir) return;

    bool denied = false;
    while (const dirent* entry = ::readdir(dir)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
        const std::string node = entry->d_name;

        bool known = false;
        for (const InputDevice& d : devices)
            if (d.node == node) known = true;
        if (known) continue;

        std::string name;
        const int fd = openIfVirtualPointer(node, name, denied);
        if (fd < 0) continue;

        devices.push_back({fd, node, name});
        qInfo() << "[LatencyFlag] watching injected clicks on" << QString::fromStdString(node)
                << "—" << QString::fromStdString(name);
    }
    ::closedir(dir);

    // Only complain while we have nothing at all: a run that is already
    // watching a pointer does not need to hear about a node it may not open.
    if (denied && devices.empty() && !g_NoInputAccess.exchange(true))
        qWarning() << "[LatencyFlag] /dev/input refused access — add this user to the `input` "
                      "group (sudo usermod -aG input $USER) and log in again, or no injected "
                      "click can be seen";
    if (!devices.empty()) g_NoInputAccess = false;
}

/// Drain a device; true when it carried a left-button press. A device that has
/// gone away reports its own death here (read fails with ENODEV).
bool readClicks(InputDevice& device, bool& goneOut)
{
    bool clicked = false;
    for (;;) {
        input_event events[32];
        const ssize_t n = ::read(device.fd, events, sizeof(events));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            goneOut = true;
            break;
        }
        if (n == 0) break;
        const size_t count = static_cast<size_t>(n) / sizeof(input_event);
        for (size_t i = 0; i < count; ++i)
            if (events[i].type == EV_KEY && events[i].code == BTN_LEFT && events[i].value == 1)
                clicked = true;
    }
    return clicked;
}

// ── The overlay: three background-filled windows per monitor ────────────────

struct FlagMonitor
{
    Window bands[3] = {0, 0, 0};
};

struct Overlay
{
    Display* dpy = nullptr;
    Window root = 0;
    int screen = 0;
    unsigned long pixels[3] = {0, 0, 0};
    std::vector<FlagMonitor> monitors;
#ifdef MW_HAVE_XRANDR
    int randrEventBase = -1;
#endif
};

/// Pure primaries, not the official tricolour: what has to survive is a
/// chroma-subsampled encode and a downscale, and the browser classifies by
/// "clearly blue / clearly white / clearly red".
void allocateColors(Overlay& ov)
{
    const unsigned short rgb[3][3] = {
        {0, 0, 65535},         // blue
        {65535, 65535, 65535}, // white
        {65535, 0, 0},         // red
    };
    Colormap cmap = DefaultColormap(ov.dpy, ov.screen);
    for (int i = 0; i < 3; ++i) {
        XColor c = {};
        c.red = rgb[i][0];
        c.green = rgb[i][1];
        c.blue = rgb[i][2];
        c.flags = DoRed | DoGreen | DoBlue;
        // A TrueColor visual never fails this; on anything older a missing
        // colour falls back to black, which the probe reads as "no flag" — a
        // wrong answer we would rather see than a wrong measurement.
        if (XAllocColor(ov.dpy, cmap, &c))
            ov.pixels[i] = c.pixel;
        else
            ov.pixels[i] = BlackPixel(ov.dpy, ov.screen);
    }
}

void destroyWindows(Overlay& ov)
{
    for (FlagMonitor& m : ov.monitors)
        for (Window w : m.bands)
            if (w) XDestroyWindow(ov.dpy, w);
    ov.monitors.clear();
}

Window createBand(Overlay& ov, int x, int y, int w, int h, unsigned long pixel)
{
    XSetWindowAttributes attrs = {};
    // Override-redirect: the window manager never sees this window, so it is
    // not decorated, not placed, not focusable and not in any task list.
    attrs.override_redirect = True;
    attrs.background_pixel = pixel;
    Window win = XCreateWindow(ov.dpy, ov.root, x, y, static_cast<unsigned>(w),
                               static_cast<unsigned>(h), 0, CopyFromParent, InputOutput,
                               CopyFromParent, CWOverrideRedirect | CWBackPixel, &attrs);
#ifdef MW_HAVE_XSHAPE
    // An empty input region: clicks meant for the application underneath pass
    // straight through, exactly like WS_EX_TRANSPARENT on Windows.
    if (win) XShapeCombineRectangles(ov.dpy, win, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
#endif
    return win;
}

/**
 * One flag per monitor, each at the same fraction of its own screen.
 *
 * The primary monitor alone is not enough: the session streams whichever
 * display the viewer picked, and a flag drawn on another one is simply absent
 * from the picture — the probe then times out on every click with no way to
 * tell that from a pipeline that never delivered.
 */
void createWindows(Overlay& ov)
{
    destroyWindows(ov);

    struct Rect
    {
        int x, y, w, h;
    };
    std::vector<Rect> rects;

#ifdef MW_HAVE_XRANDR
    int count = 0;
    if (XRRMonitorInfo* monitors = XRRGetMonitors(ov.dpy, ov.root, True, &count)) {
        for (int i = 0; i < count; ++i)
            rects.push_back({monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height});
        XRRFreeMonitors(monitors);
    }
#endif
    if (rects.empty())
        rects.push_back({0, 0, DisplayWidth(ov.dpy, ov.screen), DisplayHeight(ov.dpy, ov.screen)});

    QString description;
    for (const Rect& r : rects) {
        if (r.w <= 0 || r.h <= 0) continue;
        const int x = r.x + static_cast<int>(r.w * LatencyFlag::kLeft);
        const int y = r.y + static_cast<int>(r.h * LatencyFlag::kTop);
        const int w = static_cast<int>(r.w * (LatencyFlag::kRight - LatencyFlag::kLeft));
        const int h = static_cast<int>(r.h * (LatencyFlag::kBottom - LatencyFlag::kTop));
        if (w < 3 || h < 1) continue;

        FlagMonitor m;
        const int bandW = w / 3;
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            const int bx = x + i * bandW;
            const int bw = (i == 2) ? (x + w - bx) : bandW;
            m.bands[i] = createBand(ov, bx, y, bw, h, ov.pixels[i]);
            if (!m.bands[i]) ok = false;
        }
        if (!ok) {
            for (Window win : m.bands)
                if (win) XDestroyWindow(ov.dpy, win);
            qWarning() << "[LatencyFlag] XCreateWindow failed on a" << r.w << "x" << r.h
                       << "monitor";
            continue;
        }
        ov.monitors.push_back(m);

        description +=
            (description.isEmpty() ? "" : ", ") + QString("%1x%2 at %3,%4 on a %5x%6 screen")
                                                      .arg(w)
                                                      .arg(h)
                                                      .arg(x)
                                                      .arg(y)
                                                      .arg(r.w)
                                                      .arg(r.h);
    }

    XFlush(ov.dpy);
    if (ov.monitors.empty()) {
        qWarning() << "[LatencyFlag] no monitor to draw on";
        return;
    }
    qInfo() << "[LatencyFlag] armed on" << static_cast<int>(ov.monitors.size())
            << "screen(s):" << description << "— shown" << LatencyFlag::kShowMs
            << "ms per injected click";
}

void showAll(Overlay& ov)
{
    for (FlagMonitor& m : ov.monitors)
        for (Window w : m.bands)
            // Raise as well as map: a game that raised itself since the last
            // click would otherwise be over the flag.
            XMapRaised(ov.dpy, w);
    // Push the requests now rather than at the next loop turn: the capture may
    // run before this thread comes back to select().
    XFlush(ov.dpy);
}

void hideAll(Overlay& ov)
{
    for (FlagMonitor& m : ov.monitors)
        for (Window w : m.bands)
            XUnmapWindow(ov.dpy, w);
    XFlush(ov.dpy);
}

// ── The watcher thread ──────────────────────────────────────────────────────

void overlayThread()
{
    const char* displayName = std::getenv("DISPLAY");
    Overlay ov;
    ov.dpy = XOpenDisplay(displayName);
    if (!ov.dpy) {
        qWarning() << "[LatencyFlag] cannot open X display"
                   << (displayName ? displayName : "(unset)") << "— XAUTHORITY?";
        g_Running = false;
        return;
    }
    ov.screen = DefaultScreen(ov.dpy);
    ov.root = RootWindow(ov.dpy, ov.screen);
    allocateColors(ov);

#ifdef MW_HAVE_XRANDR
    int randrError = 0;
    // A virtual display driver arriving or leaving moves every rectangle we
    // computed — the RandR counterpart of Windows' WM_DISPLAYCHANGE.
    if (XRRQueryExtension(ov.dpy, &ov.randrEventBase, &randrError))
        XRRSelectInput(ov.dpy, ov.root, RRScreenChangeNotifyMask);
#endif

    createWindows(ov);
    if (ov.monitors.empty()) {
        XCloseDisplay(ov.dpy);
        g_Running = false;
        return;
    }

    std::vector<InputDevice> devices;
    rescanDevices(devices);

    const int xfd = ConnectionNumber(ov.dpy);
    Clock::time_point hideAt = Clock::time_point::max();
    Clock::time_point rescanAt = Clock::now() + std::chrono::milliseconds(kRescanMs);
    bool shown = false;

    for (;;) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(g_StopPipe[0], &reads);
        FD_SET(xfd, &reads);
        int maxFd = g_StopPipe[0] > xfd ? g_StopPipe[0] : xfd;
        for (const InputDevice& d : devices) {
            FD_SET(d.fd, &reads);
            if (d.fd > maxFd) maxFd = d.fd;
        }

        // Wake for whichever comes first: the hide deadline or the next rescan.
        const Clock::time_point now = Clock::now();
        Clock::time_point wakeAt = rescanAt;
        if (shown && hideAt < wakeAt) wakeAt = hideAt;
        auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(wakeAt - now).count();
        if (waitMs < 0) waitMs = 0;
        timeval tv;
        tv.tv_sec = static_cast<time_t>(waitMs / 1000);
        tv.tv_usec = static_cast<suseconds_t>((waitMs % 1000) * 1000);

        const int ready = ::select(maxFd + 1, &reads, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            qWarning() << "[LatencyFlag] select failed:" << std::strerror(errno);
            break;
        }

        if (ready > 0 && FD_ISSET(g_StopPipe[0], &reads)) break;

        if (ready > 0) {
            bool clicked = false;
            for (size_t i = 0; i < devices.size();) {
                bool gone = false;
                if (FD_ISSET(devices[i].fd, &reads) && readClicks(devices[i], gone)) clicked = true;
                if (gone) {
                    ::close(devices[i].fd);
                    devices.erase(devices.begin() + static_cast<long>(i));
                    continue;
                }
                ++i;
            }
            if (clicked) {
                showAll(ov);
                shown = true;
                // A second click restarts the countdown, as on Windows.
                hideAt = Clock::now() + std::chrono::milliseconds(LatencyFlag::kShowMs);
                // One line per injected click, so a run can be matched against
                // the browser's table (and a source that never fires shows as
                // silence).
                qInfo() << "[LatencyFlag] injected click";
            }
        }

        if (FD_ISSET(xfd, &reads) || XPending(ov.dpy) > 0) {
            while (XPending(ov.dpy) > 0) {
                XEvent event;
                XNextEvent(ov.dpy, &event);
#ifdef MW_HAVE_XRANDR
                if (ov.randrEventBase >= 0 &&
                    event.type == ov.randrEventBase + RRScreenChangeNotify) {
                    XRRUpdateConfiguration(&event);
                    createWindows(ov);
                    shown = false;
                }
#endif
            }
        }

        const Clock::time_point after = Clock::now();
        if (shown && after >= hideAt) {
            hideAll(ov);
            shown = false;
            hideAt = Clock::time_point::max();
        }
        if (after >= rescanAt) {
            rescanDevices(devices);
            rescanAt = after + std::chrono::milliseconds(kRescanMs);
        }
    }

    closeDevices(devices);
    destroyWindows(ov);
    XCloseDisplay(ov.dpy);
    qInfo() << "[LatencyFlag] stopped";
}

} // namespace

namespace LatencyFlag {

bool isSupported()
{
    if (isWaylandSession()) return false;
    return envSet("DISPLAY");
}

const char* unsupportedReason()
{
    if (isWaylandSession())
        return "this is a Wayland session — no client may draw above everything else, so the "
               "flag cannot be put on screen; run the bench in an X11 session";
    if (!envSet("DISPLAY"))
        return "no X display (DISPLAY is unset) — the flag needs a graphical session to draw on";
    if (g_NoInputAccess.load())
        return "/dev/input refused access — add this user to the `input` group "
               "(sudo usermod -aG input $USER) and log in again";
    return "";
}

bool isEnabled()
{
    return g_Running.load();
}

void setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (enabled == g_Running.load()) return;

    if (enabled) {
        if (!isSupported()) {
            qInfo() << "[LatencyFlag] not available:" << unsupportedReason();
            return;
        }
        // A thread that bailed out early (no display, no monitor) is joinable.
        if (g_Thread.joinable()) g_Thread.join();
        if (g_StopPipe[0] >= 0) {
            ::close(g_StopPipe[0]);
            ::close(g_StopPipe[1]);
            g_StopPipe[0] = g_StopPipe[1] = -1;
        }
        if (::pipe(g_StopPipe) != 0) {
            qWarning() << "[LatencyFlag] pipe failed:" << std::strerror(errno);
            return;
        }
        g_Running = true;
        g_Thread = std::thread(overlayThread);
        return;
    }

    // One byte breaks the select(); the thread tears down its own windows,
    // devices and display connection.
    if (g_StopPipe[1] >= 0) {
        const char stop = 1;
        [[maybe_unused]] const ssize_t written = ::write(g_StopPipe[1], &stop, 1);
    }
    if (g_Thread.joinable()) g_Thread.join();
    if (g_StopPipe[0] >= 0) {
        ::close(g_StopPipe[0]);
        ::close(g_StopPipe[1]);
        g_StopPipe[0] = g_StopPipe[1] = -1;
    }
    g_Running = false;
}

} // namespace LatencyFlag
