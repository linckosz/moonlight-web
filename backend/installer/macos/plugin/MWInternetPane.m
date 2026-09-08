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

@implementation MWInternetPane {
    BOOL _built;
    NSButton *_internetCheck;
}

// Sidebar label comes from InstallerSectionTitle; this is the pane title shown
// above the content area.
- (NSString *)title { return @"Internet"; }

- (NSTextField *)labelAt:(CGFloat)y height:(CGFloat)h text:(NSString *)s in:(NSView *)view
{
    NSTextField *l = [[NSTextField alloc] initWithFrame:NSMakeRect(0, y, 470, h)];
    l.stringValue = s;
    l.bezeled = NO; l.drawsBackground = NO; l.editable = NO; l.selectable = NO;
    // Explicit wrapping: the explanatory blurbs run over several lines.
    l.usesSingleLineMode = NO;
    l.lineBreakMode = NSLineBreakByWordWrapping;
    [view addSubview:l];
    return l;
}

// The nib provides an empty contentView (real InstallerPane IBOutlet); populate
// it once. Outlets are connected before awakeFromNib fires.
- (void)awakeFromNib
{
    [super awakeFromNib];
    if (_built) return;
    NSView *view = [self contentView];
    if (!view) return;
    _built = YES;

    [self labelAt:170
            height:56
              text:@"MoonlightWeb streams this Mac itself — there is nothing else to "
                   @"install.\n\nOne question before it does:"
                in:view];

    // Pre-ticked only when a previous install already authorized Internet access
    // (settings.json) — a re-install must not silently forget the prior opt-in.
    // First install stays unchecked: opening the machine to the Internet
    // (per-session UPnP mapping) requires an explicit opt-in click. The label IS
    // the recorded consent, so it wraps over several lines to say what enabling
    // does and does not do; a discreet positive green tint draws the eye.
    //
    // The whole band the credentials used to occupy is its own now, which is the
    // one improvement this simplification buys the user: the agreement they are
    // recorded as having read is no longer squeezed into 46 points.
    _internetCheck = [[NSButton alloc] initWithFrame:NSMakeRect(0, 30, 470, 130)];
    [_internetCheck setButtonType:NSButtonTypeSwitch];
    NSMutableParagraphStyle *wrap = [[NSMutableParagraphStyle alloc] init];
    wrap.lineBreakMode = NSLineBreakByWordWrapping;
    _internetCheck.attributedTitle = [[NSAttributedString alloc]
        initWithString:MWInternetConsentText()
            attributes:@{
                NSForegroundColorAttributeName : [NSColor systemGreenColor],
                NSFontAttributeName : [NSFont systemFontOfSize:11],
                NSParagraphStyleAttributeName : wrap
            }];
    _internetCheck.state =
        MWInternetAlreadyAuthorized() ? NSControlStateValueOn : NSControlStateValueOff;
    [view addSubview:_internetCheck];
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
