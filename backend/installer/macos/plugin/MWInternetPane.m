/*
 * MoonlightWeb — Installer.app plugin: single "Internet" pane.
 *
 * One question, the same one the Windows Inno installer asks: may this Mac be
 * reachable from outside the local network? The answer (and the exact wording
 * it was given for) is handed to the .pkg postinstall through the /tmp plist
 * (see MWCommon.h), which writes it into provisioning.json for the server's
 * first run.
 *
 * This pane used to collect Sunshine credentials and download its DMG in the
 * background. It does not any more: the app captures and encodes this Mac
 * itself (ScreenCaptureKit → VideoToolbox, design §20), so a fresh install has
 * no second streaming server to set up — and no password to carry through /tmp
 * in plaintext. Sunshine stays a first-class HOST: a Mac that already runs it
 * is discovered and paired from the hosts page, on the user's initiative.
 *
 * The one thing an installer cannot arrange is Screen Recording — TCC grants it
 * by hand only, and only to a program that is already running. The postinstall
 * launches the app for exactly that reason, and the in-app wizard says what to
 * tick if it is still missing.
 *
 * The nib (MWInternetPane.xib) only carries the template-mandated wiring
 * (section.firstPane -> pane, pane.contentView -> empty view); the controls are
 * built programmatically in awakeFromNib so no Interface Builder is needed
 * beyond a plain ibtool compile.
 */
#import "MWInternetPane.h"
#import "MWCommon.h"
#import "MWInternetPaneContent.h"

@implementation MWInternetPane {
    BOOL _built;
    NSButton *_internetCheck;
}

// Sidebar label comes from InstallerSectionTitle; this is the pane title shown
// above the content area.
- (NSString *)title { return @"Internet"; }

// The nib provides an empty contentView (real InstallerPane IBOutlet); populate
// it once. Outlets are connected before awakeFromNib fires. The layout itself
// lives in MWInternetPaneContent.h so a preview tool can render exactly these
// views offscreen — see that header.
- (void)awakeFromNib
{
    [super awakeFromNib];
    if (_built) return;
    NSView *view = [self contentView];
    if (!view) return;
    _built = YES;

    _internetCheck = MWBuildInternetPaneContent(view);
    // Pre-ticked only when a previous install already authorized Internet access
    // (settings.json) — a re-install must not silently forget the prior opt-in.
    // First install stays unchecked: opening the machine to the Internet
    // (per-session UPnP mapping) requires an explicit opt-in click.
    _internetCheck.state =
        MWInternetAlreadyAuthorized() ? NSControlStateValueOn : NSControlStateValueOff;
}

// Everything the postinstall needs. Written on the way out, and again from
// didEnterPane's backward direction is not needed: the pane holds no state the
// user can lose.
- (void)writeHandoff
{
    MWHandoffMerge(@{
        @"internet" : @(_internetCheck.state == NSControlStateValueOn),
        // Exact agreement text displayed — recorded by the server in its DNS
        // registration audit log (legal traceability).
        @"consent" : MWInternetConsentText(),
    });
}

- (BOOL)shouldExitPane:(InstallerSectionDirection)dir
{
    if (dir != InstallerDirectionForward) return YES;
    [self writeHandoff];
    return YES;
}

@end
