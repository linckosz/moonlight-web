/*
 * MoonlightWeb — Installer.app plugin: single "Sunshine" pane.
 *
 * Collects the same choices the Windows Inno installer does — Sunshine username/
 * password + "allow the public Internet link" — and, in the background, downloads
 * the Sunshine DMG with a real progress bar (the privileged copy to /Applications
 * happens in the .pkg postinstall, which runs as root). Everything is handed to
 * the postinstall through the /tmp plist (see MWCommon.h).
 *
 * The pane has three shapes, matching the Windows wizard page:
 *   MWSunshineAbsent   — Sunshine not installed: download it in the background and
 *                        collect the credentials it will be created with,
 *                        prefilled admin/admin (the password shows in clear until
 *                        the user edits it, then it becomes a secure field).
 *   MWSunshineUnpaired — Sunshine installed, never paired with MoonlightWeb: ask
 *                        for its REAL credentials so first-run pairing can push
 *                        the PIN through Sunshine's REST API. Nothing to download.
 *   MWSunshinePaired   — Sunshine installed AND already paired: a plain message,
 *                        no field, no choice, and no Skip button.
 *
 * Installer.app's own buttons (Go Back / Continue / Cancel) cannot be extended,
 * so "Skip" is an in-pane button that clears the credentials and advances.
 *
 * The nib (MWSunshinePane.xib) only carries the template-mandated wiring
 * (section.firstPane -> pane, pane.contentView -> empty view); the controls are
 * built programmatically in awakeFromNib so no Interface Builder is needed
 * beyond a plain ibtool compile. Pairing + the public A-record are NOT done here:
 * they need the app installed and running, so MoonlightWeb.app does them on first
 * launch (Provisioning::applyOnce) and shows the live checklist in the browser.
 */
#import "MWSunshinePane.h"
#import "MWCommon.h"

typedef NS_ENUM(NSInteger, MWSunshineCase) {
    MWSunshineAbsent = 0,
    MWSunshineUnpaired,
    MWSunshinePaired,
};

@interface MWSunshinePane () <NSURLSessionDownloadDelegate, NSTextFieldDelegate>
@end

@implementation MWSunshinePane {
    BOOL _built;
    MWSunshineCase _case;
    NSTextField *_userField;
    // The password is either a plain field (prefilled "admin", readable) or a
    // secure one; exactly one of the two is in the view hierarchy at a time.
    NSTextField *_passPlain;
    NSSecureTextField *_passSecure;
    NSButton *_internetCheck;
    NSButton *_skipButton;
    NSTextField *_statusLabel;
    NSProgressIndicator *_progress;
    NSURLSessionDownloadTask *_task;
    BOOL _started;
    BOOL _skipped;
}

// Sidebar label comes from InstallerSectionTitle; this is the pane title shown
// above the content area.
- (NSString *)title { return @"Sunshine"; }

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

- (NSTextField *)labelAt:(CGFloat)y text:(NSString *)s in:(NSView *)view
{
    return [self labelAt:y height:18 text:s in:view];
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

    if (MWSunshineInstalledPath() == nil)
        _case = MWSunshineAbsent;
    else
        _case = MWLocalSunshinePaired() ? MWSunshinePaired : MWSunshineUnpaired;

    if (_case == MWSunshinePaired) {
        [self buildPairedView:view];
        return;
    }
    [self buildCredentialsView:view];
}

// Already installed AND already paired: nothing to install, nothing to ask. A
// single message, and the chrome's Continue / Cancel are the only way out.
- (void)buildPairedView:(NSView *)view
{
    [self labelAt:150
            height:80
              text:@"Sunshine is already installed on this Mac and already paired with "
                   @"MoonlightWeb.\n\nThere is nothing to set up here — click Continue."
                in:view];

    // The Internet opt-in is not a Sunshine choice: it stays offered.
    [self addInternetCheckAt:110 in:view];
}

- (void)addInternetCheckAt:(CGFloat)y in:(NSView *)view
{
    // Pre-ticked only when a previous install already authorized Internet access
    // (settings.json) — a re-install must not silently forget the prior opt-in.
    // First install stays unchecked: opening the machine to the Internet (UPnP +
    // public DNS record) requires an explicit opt-in click. The label carries a
    // discreet positive green tint to draw the eye.
    _internetCheck = [[NSButton alloc] initWithFrame:NSMakeRect(0, y, 470, 22)];
    [_internetCheck setButtonType:NSButtonTypeSwitch];
    _internetCheck.attributedTitle = [[NSAttributedString alloc]
        initWithString:MWInternetConsentText()
            attributes:@{NSForegroundColorAttributeName : [NSColor systemGreenColor]}];
    _internetCheck.state =
        MWInternetAlreadyAuthorized() ? NSControlStateValueOn : NSControlStateValueOff;
    [view addSubview:_internetCheck];
}

- (void)buildCredentialsView:(NSView *)view
{
    if (_case == MWSunshineAbsent) {
        [self labelAt:194
                height:36
                  text:@"Sunshine was not found on this Mac and will be installed. These are "
                       @"the credentials it will be created with — change them if you prefer."
                    in:view];
    } else {
        [self labelAt:194
                height:36
                  text:@"Sunshine is already installed. Enter its credentials so MoonlightWeb "
                       @"can pair automatically, or click Skip to pair later."
                    in:view];
    }

    [self labelAt:168 text:@"Username" in:view];
    _userField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 144, 220, 22)];
    // A fresh install gets admin/admin; an existing one must be told its real
    // username, so prefilling it there would only invite a failed PIN push.
    _userField.stringValue = (_case == MWSunshineAbsent) ? @"admin" : @"";
    [view addSubview:_userField];

    [self labelAt:116 text:@"Password" in:view];
    NSRect passFrame = NSMakeRect(0, 92, 220, 22);
    if (_case == MWSunshineAbsent) {
        // The prefilled default is a value to read, not a secret to hide — until
        // the user makes it one (controlTextDidChange: swaps in a secure field).
        _passPlain = [[NSTextField alloc] initWithFrame:passFrame];
        _passPlain.stringValue = @"admin";
        _passPlain.delegate = self;
        [view addSubview:_passPlain];
    } else {
        _passSecure = [[NSSecureTextField alloc] initWithFrame:passFrame];
        [view addSubview:_passSecure];
    }

    [self addInternetCheckAt:58 in:view];

    // Status + progress stop short of the Skip button's column.
    _statusLabel = [self labelAt:32 text:@"Preparing Sunshine…" in:view];
    _statusLabel.frame = NSMakeRect(0, 32, 360, 18);
    _statusLabel.textColor = [NSColor secondaryLabelColor];
    _statusLabel.hidden = (_case != MWSunshineAbsent);

    _progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 12, 360, 16)];
    _progress.style = NSProgressIndicatorStyleBar;
    _progress.indeterminate = NO;
    _progress.minValue = 0.0;
    _progress.maxValue = 1.0;
    _progress.hidden = (_case != MWSunshineAbsent);
    [view addSubview:_progress];

    // "Skip": leave Sunshine alone (no install, no pairing) and move on. The
    // admin page can still install/pair later.
    _skipButton = [[NSButton alloc] initWithFrame:NSMakeRect(380, 6, 90, 28)];
    _skipButton.bezelStyle = NSBezelStyleRounded;
    _skipButton.title = @"Skip";
    _skipButton.target = self;
    _skipButton.action = @selector(skipClicked:);
    [view addSubview:_skipButton];
}

// Password value from whichever field is currently live.
- (NSString *)passwordValue
{
    return _passSecure ? _passSecure.stringValue : (_passPlain ? _passPlain.stringValue : @"");
}

#pragma mark - Password reveal → mask

// First edit of the prefilled password turns the plain field into a real secure
// one. Deferred to the next runloop turn: swapping the view that owns the active
// field editor from inside its own change notification is not safe.
- (void)controlTextDidChange:(NSNotification *)note
{
    if (note.object != _passPlain || _passSecure != nil) return;
    NSTextField *plain = _passPlain;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_passSecure != nil) return;
        NSView *view = plain.superview;
        if (!view) return;

        NSString *text = plain.stringValue;
        self->_passSecure = [[NSSecureTextField alloc] initWithFrame:plain.frame];
        self->_passSecure.stringValue = text;
        [view addSubview:self->_passSecure];
        [plain removeFromSuperview];
        self->_passPlain = nil;

        // Keep typing where the user left off.
        [view.window makeFirstResponder:self->_passSecure];
        NSText *editor = [view.window fieldEditor:YES forObject:self->_passSecure];
        [editor setSelectedRange:NSMakeRange(text.length, 0)];
    });
}

#pragma mark - Sunshine download

// Start the Sunshine download once, when the pane first appears. It runs while
// the user fills in the credentials; the progress bar reflects it. Nothing to
// fetch when Sunshine is already installed.
- (void)didEnterPane:(InstallerSectionDirection)dir
{
    // Coming back to the pane cancels an earlier Skip: what the fields hold now
    // is what counts again.
    _skipped = NO;
    if (_started || _case != MWSunshineAbsent) return;
    _started = YES;

    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg delegate:self
                                                     delegateQueue:nil];
    NSURL *url = [NSURL URLWithString:MWSunshineDmgURL()];
    _task = [session downloadTaskWithURL:url];
    [_task resume];
}

#pragma mark - NSURLSessionDownloadDelegate

- (void)URLSession:(NSURLSession *)session
              downloadTask:(NSURLSessionDownloadTask *)task
              didWriteData:(int64_t)bytesWritten
         totalBytesWritten:(int64_t)total
 totalBytesExpectedToWrite:(int64_t)expected
{
    if (expected <= 0) return;
    double frac = (double)total / (double)expected;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_progress.doubleValue = frac;
        self->_statusLabel.stringValue =
            [NSString stringWithFormat:@"Downloading Sunshine… %d%%", (int)(frac * 100)];
    });
}

- (void)URLSession:(NSURLSession *)session
         downloadTask:(NSURLSessionDownloadTask *)task
didFinishDownloadingToURL:(NSURL *)location
{
    NSString *dest = @"/tmp/mw-sunshine.dmg";
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtPath:dest error:nil];
    NSError *err = nil;
    BOOL ok = [fm moveItemAtURL:location toURL:[NSURL fileURLWithPath:dest] error:&err];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (ok) {
            MWHandoffMerge(@{ @"dmg" : dest });
            self->_progress.doubleValue = 1.0;
            self->_statusLabel.stringValue = @"Sunshine downloaded.";
        } else {
            self->_statusLabel.stringValue = @"Sunshine will be downloaded during installation.";
        }
    });
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error
{
    if (!error) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_progress.doubleValue = 0.0;
        // A cancelled download is the user skipping the step, not a failure.
        if (!self->_skipped)
            self->_statusLabel.stringValue = @"Sunshine will be downloaded during installation.";
    });
}

#pragma mark - Navigation

// Everything the postinstall needs. `install` / `autopair` are what Skip and the
// already-paired case turn off; the internet opt-in is independent of Sunshine
// and is always recorded.
- (void)writeHandoff
{
    BOOL wantsSunshine = (_case == MWSunshineAbsent) && !_skipped;
    BOOL wantsPairing = (_case != MWSunshinePaired) && !_skipped;
    NSString *user = wantsPairing ? [_userField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] : @"";
    NSString *pass = wantsPairing ? [self passwordValue] : @"";

    MWHandoffMerge(@{
        @"install" : @(wantsSunshine),
        @"autopair" : @(wantsPairing),
        @"username" : user,
        @"password" : pass,
        @"internet" : @(_internetCheck.state == NSControlStateValueOn),
        // Exact agreement text displayed — recorded by the server in its DNS
        // registration audit log (legal traceability).
        @"consent" : MWInternetConsentText(),
    });
}

- (void)skipClicked:(id)sender
{
    _skipped = YES;
    [_task cancel];
    _task = nil;
    _started = NO; // so coming back to the pane restarts the download
    _statusLabel.stringValue = @"Sunshine setup skipped.";
    // Write it now: gotoNextPane is not guaranteed to route through
    // shouldExitPane:, and the hand-off must never be left half-written.
    [self writeHandoff];
    [self gotoNextPane];
}

// Credentials are only mandatory when they will actually be used: a fresh
// install applies them via `sunshine --creds`, an existing install needs its
// real ones to accept the PIN. Skip and the already-paired case need neither.
- (BOOL)shouldExitPane:(InstallerSectionDirection)dir
{
    if (dir != InstallerDirectionForward) return YES;

    if (!_skipped && _case != MWSunshinePaired) {
        NSString *user = [_userField.stringValue
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (user.length == 0 || [self passwordValue].length == 0) {
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = @"Sunshine credentials required";
            a.informativeText = @"Enter a username and password so MoonlightWeb can pair with "
                                @"Sunshine automatically, or click Skip to do it later.";
            [a runModal];
            return NO;
        }
    }

    [self writeHandoff];
    return YES;
}

@end
