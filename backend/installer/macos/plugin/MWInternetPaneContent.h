/*
 * MoonlightWeb — the Internet pane's content, built in one place.
 *
 * Separated from MWInternetPane.m for one reason: an InstallerPane can only be
 * seen by running Installer.app, and Installer.app cannot be driven remotely on
 * the bench (design §25). Putting the layout here lets a tiny preview tool
 * render the very same views offscreen to a PNG — so what is reviewed is what
 * ships, not a re-typed imitation of it.
 */
#import <Cocoa/Cocoa.h>

#import "MWCommon.h"

// ⚠️ Never lay this pane out with fixed frames. The first version placed
// everything in 470-point-wide rectangles because the nib's content view is
// 470x240; Installer.app gives the pane a NARROWER area than that, and the
// labels — fixed size, no autoresizing — kept their width and were clipped on
// the right, mid-word (seen on a real install, 09/09/2026). Auto layout against
// the view we are actually given cannot make that mistake, whatever size the
// pane ends up with.
static inline NSTextField *MWWrappingLabel(NSString *text, NSColor *color, CGFloat size)
{
    NSTextField *l = [NSTextField wrappingLabelWithString:text];
    l.translatesAutoresizingMaskIntoConstraints = NO;
    l.font = [NSFont systemFontOfSize:size];
    l.textColor = color;
    l.selectable = NO;
    return l;
}

/// Fill `view` with the pane's contents and return the opt-in checkbox, whose
/// state the caller reads on the way out.
static inline NSButton *MWBuildInternetPaneContent(NSView *view)
{
    NSTextField *intro =
        MWWrappingLabel(@"MoonlightWeb streams this Mac itself — there is nothing else to "
                        @"install.\n\nOne question before it does:",
                        [NSColor labelColor], [NSFont systemFontSize]);

    // The checkbox carries the sentence the user ticks; the paragraph below it
    // carries the rest of the same agreement. ⚠️ The first version put the whole
    // thing in the checkbox's own title, in light green: six wrapped lines of
    // systemGreenColor on the installer's white pane, which Bruno read as
    // "cassé et pas lisible" and was right about. A control's title is for the
    // thing being decided; the reasoning belongs in body text, in the system's
    // own label colours, which are contrast-checked for us.
    NSButton *check = [[NSButton alloc] init];
    [check setButtonType:NSButtonTypeSwitch];
    check.translatesAutoresizingMaskIntoConstraints = NO;
    check.title = MWInternetConsentLead();
    check.font = [NSFont systemFontOfSize:[NSFont systemFontSize]];

    NSTextField *body = MWWrappingLabel(MWInternetConsentBody(), [NSColor secondaryLabelColor],
                                        [NSFont smallSystemFontSize]);

    NSStackView *stack = [NSStackView stackViewWithViews:@[ intro, check, body ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [stack setCustomSpacing:6 afterView:check];
    [view addSubview:stack];

    // Pinned on three sides and free at the bottom: the pane is as tall as its
    // text needs, and as wide as Installer chose to make it.
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:view.topAnchor constant:12],
        [stack.leadingAnchor constraintEqualToAnchor:view.leadingAnchor constant:0],
        [stack.trailingAnchor constraintEqualToAnchor:view.trailingAnchor constant:0],
        // Without these the labels would size to their (very long) single line
        // and push the stack wider than the pane instead of wrapping inside it.
        [intro.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [body.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
    ]];
    return check;
}
