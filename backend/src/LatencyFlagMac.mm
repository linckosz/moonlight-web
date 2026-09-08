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
 * Click-to-photon flag, macOS backend. The contract, the geometry and the
 * colours are LatencyFlag.h's; only the two mechanisms differ from Windows.
 *
 * ── Seeing the click ────────────────────────────────────────────────────────
 *
 * A listen-only CGEventTap on the session, on its own thread with its own run
 * loop. Listen-only matters: the tap never delays, alters or swallows the click
 * it observes, so the thing being measured is not perturbed by the measuring.
 *
 * "Injected" is kCGEventSourceStateID. A click from a real mouse carries
 * kCGEventSourceStateHIDSystemState; one posted by CGEventPost — which is how
 * Sunshine, our own native host and every other host on this machine inject —
 * carries the source's own state, never the HID one. That single integer is the
 * exact equivalent of Windows' LLMHF_INJECTED.
 *
 * The tap needs Input Monitoring for *this binary*. Without it the tap is
 * created happily and then never fires, which is indistinguishable from a
 * pipeline that delivers nothing — so the preflight is answered in
 * unsupportedReason() rather than left to be discovered at measurement time.
 * TCC keys the grant to the code signature, so the bench's stable "MoonlightWeb
 * Dev" identity is what keeps it across rebuilds (an ad-hoc signature changes
 * every build and loses it).
 *
 * ── Showing the flag ────────────────────────────────────────────────────────
 *
 * One borderless NSWindow per NSScreen, at the same fraction of each screen as
 * everywhere else, above everything (CGShieldingWindowLevel), ignoring mouse
 * events, and joining every Space so a full-screen game does not hide it.
 *
 * AppKit windows may only be touched from the main thread, so the tap thread
 * does no drawing: it hands a "show" to the main queue. That hop is a dispatch
 * onto an idle run loop — microseconds — and it is on the same side of the
 * measurement as the OS's own input delivery, so it does not bias the figure in
 * a way the Windows path avoids.
 *
 * Hiding is an epoch, not a timer: every show bumps a counter and schedules a
 * check kShowMs later, which hides only if no newer show happened since. Two
 * clicks close together therefore extend the flag rather than cutting it short,
 * which is what the Windows timer does by being reset.
 *
 * ScreenCaptureKit captures these windows: the native host's filter is built
 * with excludingWindows:@[] (SckCapture.mm), so nothing of ours is filtered out
 * of the picture. Sunshine on this machine captures the display too.
 */

#include "LatencyFlag.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>

#include <QDebug>
#include <QString>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// ── The flag view: three flat bands, drawn by AppKit ────────────────────────

@interface MwLatencyFlagView : NSView
@end

@implementation MwLatencyFlagView

- (BOOL)isOpaque
{
    return YES;
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    const CGFloat bandW = b.size.width / 3.0;
    // Device RGB, deliberately: what has to survive is a chroma-subsampled
    // encode and a downscale, and the browser classifies "clearly blue /
    // clearly white / clearly red". Going through a colour-managed space would
    // buy accuracy nobody reads and lose saturation the thresholds want.
    NSColor* colors[3] = {
        [NSColor colorWithDeviceRed:0.0 green:0.0 blue:1.0 alpha:1.0],
        [NSColor colorWithDeviceRed:1.0 green:1.0 blue:1.0 alpha:1.0],
        [NSColor colorWithDeviceRed:1.0 green:0.0 blue:0.0 alpha:1.0],
    };
    for (int i = 0; i < 3; ++i) {
        NSRect band = b;
        band.origin.x = b.origin.x + i * bandW;
        band.size.width = (i == 2) ? (b.origin.x + b.size.width - band.origin.x) : bandW;
        [colors[i] setFill];
        NSRectFill(band);
    }
}

@end

namespace {

std::mutex g_Mutex;
std::atomic<bool> g_Running{false};
std::atomic<bool> g_TapLive{false};

// Owned by the main thread once created; the tap thread only ever posts to the
// main queue, so no lock guards them.
NSMutableArray* g_Windows = nil;
id g_ScreenObserver = nil;

// Bumped by every show; the delayed hide only fires if it is still the latest.
std::atomic<unsigned long long> g_ShowEpoch{0};

// The tap thread's run loop, so setEnabled(false) can stop it from outside.
std::atomic<CFRunLoopRef> g_RunLoop{nullptr};
std::thread g_Thread;
CFMachPortRef g_Tap = nullptr;
CFRunLoopSourceRef g_Source = nullptr;

/// Run a block on the main thread. AppKit demands it for every window call, and
/// setEnabled() can arrive from either the startup path (main thread) or an
/// HTTP handler (a server thread) — dispatch_sync from the main thread would
/// deadlock, so the caller's thread is checked rather than assumed.
void runOnMain(void (^block)(void), bool wait)
{
    if ([NSThread isMainThread]) {
        block();
        return;
    }
    if (wait)
        dispatch_sync(dispatch_get_main_queue(), block);
    else
        dispatch_async(dispatch_get_main_queue(), block);
}

void destroyWindowsOnMain()
{
    for (NSWindow* w in g_Windows) {
        [w orderOut:nil];
        [w close];
    }
    [g_Windows release];
    g_Windows = nil;
}

/**
 * One flag per screen, each at the same fraction of its own screen.
 *
 * The main screen alone is not enough: the session streams whichever display
 * the viewer picked, and a flag drawn on another one is simply absent from the
 * picture — the probe then times out on every click with no way to tell that
 * from a pipeline that never delivered.
 *
 * NSScreen frames are in points with the origin at the bottom-left of the main
 * screen, while the flag belongs at the TOP of each screen (with tearing the
 * top rows are the freshest scanned out), hence the flip against kBottom.
 * Points, not pixels: on a Retina screen the fraction is what matters, and it
 * survives the scale untouched.
 */
void createWindowsOnMain()
{
    destroyWindowsOnMain();
    g_Windows = [[NSMutableArray alloc] init];

    NSMutableString* description = [NSMutableString string];
    for (NSScreen* screen in [NSScreen screens]) {
        const NSRect f = [screen frame];
        if (f.size.width <= 0 || f.size.height <= 0) continue;

        NSRect r;
        r.origin.x = f.origin.x + f.size.width * LatencyFlag::kLeft;
        r.size.width = f.size.width * (LatencyFlag::kRight - LatencyFlag::kLeft);
        r.size.height = f.size.height * (LatencyFlag::kBottom - LatencyFlag::kTop);
        r.origin.y = f.origin.y + f.size.height * (1.0 - LatencyFlag::kBottom);

        NSWindow* win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        // Above full-screen games and the menu bar alike. Not a "shielding
        // window" in the security sense — just its level.
        [win setLevel:CGShieldingWindowLevel()];
        // Never take a click meant for the app underneath, never take focus,
        // never appear in Exposé or the window menu.
        [win setIgnoresMouseEvents:YES];
        [win setOpaque:YES];
        [win setHasShadow:NO];
        [win setExcludedFromWindowsMenu:YES];
        [win setReleasedWhenClosed:NO];
        [win setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorStationary |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary |
                                    NSWindowCollectionBehaviorIgnoresCycle)];

        MwLatencyFlagView* view =
            [[MwLatencyFlagView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width, r.size.height)];
        [win setContentView:view];
        [view release];

        [g_Windows addObject:win];
        [win release]; // the array owns it now

        [description appendFormat:@"%@%.0fx%.0f at %.0f,%.0f on a %.0fx%.0f screen",
                                  ([description length] ? @", " : @""), r.size.width, r.size.height,
                                  r.origin.x, r.origin.y, f.size.width, f.size.height];
    }

    if ([g_Windows count] == 0) {
        qWarning() << "[LatencyFlag] no screen to draw on";
        return;
    }
    qInfo() << "[LatencyFlag] armed on" << static_cast<int>([g_Windows count])
            << "screen(s):" << QString::fromNSString(description) << "— shown"
            << LatencyFlag::kShowMs << "ms per injected click";
}

void showAllOnMain()
{
    for (NSWindow* w in g_Windows) {
        // orderFrontRegardless, not makeKeyAndOrderFront: showing the flag must
        // not move focus away from whatever the click landed on.
        [w orderFrontRegardless];
        // Draw now rather than at the next cycle: the capture may run before
        // AppKit gets back to its own display pass otherwise.
        [[w contentView] displayIfNeeded];
    }
}

void hideAllOnMain()
{
    for (NSWindow* w in g_Windows)
        [w orderOut:nil];
}

/// Show, and schedule the hide. Called from the tap thread.
void flash()
{
    const unsigned long long epoch = ++g_ShowEpoch;
    runOnMain(
        ^{
            @autoreleasepool {
                showAllOnMain();
            }
        },
        false);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 static_cast<int64_t>(LatencyFlag::kShowMs) * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
                       @autoreleasepool {
                           // A newer click has re-shown the flag since: let its own hide run.
                           if (g_ShowEpoch.load() == epoch) hideAllOnMain();
                       }
                   });
}

CGEventRef tapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* userInfo)
{
    (void)proxy;
    (void)userInfo;

    // The system disables a tap that misbehaves or that the user interrupted.
    // A disabled tap is silent, which reads exactly like "the host injects
    // nothing" — re-arm it instead of going quietly blind.
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (g_Tap) CGEventTapEnable(g_Tap, true);
        return event;
    }

    if (type == kCGEventLeftMouseDown) {
        const int64_t state = CGEventGetIntegerValueField(event, kCGEventSourceStateID);
        const bool injected = state != kCGEventSourceStateHIDSystemState;
        if (injected) {
            flash();
            // One line per injected click, so a run can be matched against the
            // browser's table (and a tap that never fires shows as silence).
            const CGPoint p = CGEventGetLocation(event);
            qInfo() << "[LatencyFlag] injected click at" << static_cast<int>(p.x) << ","
                    << static_cast<int>(p.y);
        }
    }
    return event;
}

void tapThread()
{
    @autoreleasepool {
        g_Tap =
            CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
                             CGEventMaskBit(kCGEventLeftMouseDown), tapCallback, nullptr);
        if (!g_Tap) {
            qWarning() << "[LatencyFlag] CGEventTapCreate failed — Input Monitoring is not "
                          "granted to this binary (System Settings › Privacy & Security › "
                          "Input Monitoring)";
            g_Running = false;
            return;
        }

        g_Source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_Tap, 0);
        CFRunLoopAddSource(CFRunLoopGetCurrent(), g_Source, kCFRunLoopCommonModes);
        CGEventTapEnable(g_Tap, true);

        g_RunLoop.store(CFRunLoopGetCurrent());
        g_TapLive = true;
        CFRunLoopRun();
        g_TapLive = false;
        g_RunLoop.store(nullptr);

        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_Source, kCFRunLoopCommonModes);
        CFRelease(g_Source);
        g_Source = nullptr;
        CGEventTapEnable(g_Tap, false);
        CFRelease(g_Tap);
        g_Tap = nullptr;
    }
}

/// Input Monitoring, asked without prompting. macOS 10.15+; older systems put
/// mouse taps under Accessibility, which the host already holds to inject.
bool listenAccessGranted()
{
    if (@available(macOS 10.15, *)) return CGPreflightListenEventAccess();
    return true;
}

} // namespace

namespace LatencyFlag {

bool isSupported()
{
    // The backend exists on every macOS the app runs on. Whether TCC will let
    // the tap fire is a grant, not a capability — unsupportedReason() carries
    // that, so the switch stays offered and turning it on is what asks for the
    // permission.
    return true;
}

const char* unsupportedReason()
{
    if (!listenAccessGranted())
        return "macOS has not granted Input Monitoring to this binary — System Settings › "
               "Privacy & Security › Input Monitoring; the flag cannot see injected clicks "
               "until it is ticked";
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
        if (!listenAccessGranted()) {
            // Ask once. The dialog (or the entry in the list, on a binary macOS
            // has already seen) is the only way the operator can grant it, and
            // an unasked permission would leave the tap silent forever.
            if (@available(macOS 10.15, *)) CGRequestListenEventAccess();
            qWarning() << "[LatencyFlag]" << unsupportedReason();
        }

        if (g_Thread.joinable()) g_Thread.join(); // a thread that bailed out early
        g_Running = true;

        runOnMain(
            ^{
                @autoreleasepool {
                    createWindowsOnMain();
                    // A display arriving or leaving moves every rectangle computed
                    // above — the same reason Windows rebuilds on WM_DISPLAYCHANGE.
                    // Bench machines with a virtual display make that the normal case.
                    if (!g_ScreenObserver) {
                        g_ScreenObserver = [[[NSNotificationCenter defaultCenter]
                            addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                        object:nil
                                         queue:[NSOperationQueue mainQueue]
                                    usingBlock:^(NSNotification* note) {
                                        (void)note;
                                        if (g_Running.load()) createWindowsOnMain();
                                    }] retain];
                    }
                }
            },
            true);

        g_Thread = std::thread(tapThread);
        return;
    }

    CFRunLoopRef loop = g_RunLoop.load();
    // The thread may not have reached CFRunLoopRun() yet: retry briefly rather
    // than stopping a run loop that does not exist, which would leave it running.
    for (int i = 0; i < 100 && loop == nullptr && g_Thread.joinable(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        loop = g_RunLoop.load();
    }
    if (loop) CFRunLoopStop(loop);
    if (g_Thread.joinable()) g_Thread.join();

    runOnMain(
        ^{
            @autoreleasepool {
                if (g_ScreenObserver) {
                    [[NSNotificationCenter defaultCenter] removeObserver:g_ScreenObserver];
                    [g_ScreenObserver release];
                    g_ScreenObserver = nil;
                }
                destroyWindowsOnMain();
            }
        },
        true);

    g_Running = false;
    qInfo() << "[LatencyFlag] stopped";
}

} // namespace LatencyFlag
