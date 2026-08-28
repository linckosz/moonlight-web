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
    // Credential check (MWSunshineUnpaired only): Continue starts it, the pane
    // stays put until Sunshine has answered, and only an accepted pair advances.
    NSURLSessionDataTask *_checkTask;
    BOOL _checking;
    BOOL _credsVerified;
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
    [self addInternetCheckAt:98 in:view];
}

- (void)addInternetCheckAt:(CGFloat)y in:(NSView *)view
{
    // Pre-ticked only when a previous install already authorized Internet access
    // (settings.json) — a re-install must not silently forget the prior opt-in.
    // First install stays unchecked: opening the machine to the Internet
    // (per-session UPnP mapping) requires an explicit opt-in click. The label IS
    // the recorded consent, so it wraps over several lines to say what enabling
    // does and does not do; a discreet positive green tint draws the eye.
    _internetCheck = [[NSButton alloc] initWithFrame:NSMakeRect(0, y, 470, 46)];
    [_internetCheck setButtonType:NSButtonTypeSwitch];
    NSMutableParagraphStyle *wrap = [[NSMutableParagraphStyle alloc] init];
    wrap.lineBreakMode = NSLineBreakByWordWrapping;
    _internetCheck.attributedTitle = [[NSAttributedString alloc]
        initWithString:MWInternetConsentText()
            attributes:@{
                NSForegroundColorAttributeName : [NSColor systemGreenColor],
                NSFontAttributeName : [NSFont systemFontOfSize:10],
                NSParagraphStyleAttributeName : wrap
            }];
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
    _userField.delegate = self;
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
        _passSecure.delegate = self;
        [view addSubview:_passSecure];
    }

    // The taller multi-line consent checkbox (46px) fills the band between the
    // password field (bottom edge y=92) and the status line below.
    [self addInternetCheckAt:46 in:view];

    // Status + progress stop short of the Skip button's column.
    _statusLabel = [self labelAt:24 text:@"Preparing Sunshine…" in:view];
    _statusLabel.frame = NSMakeRect(0, 24, 360, 18);
    _statusLabel.textColor = [NSColor secondaryLabelColor];
    _statusLabel.hidden = (_case != MWSunshineAbsent);

    _progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 6, 360, 16)];
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
    // Any edit invalidates a previous check — Continue has to ask Sunshine again
    // — and clears whatever verdict is still on screen.
    _credsVerified = NO;
    if (_case == MWSunshineUnpaired && !_checking) [self hideCheckStatus];

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

#pragma mark - Credential check

// Layout note: the status label + progress bar are the download indicators of
// the MWSunshineAbsent case, reused here — the two cases never run together.
- (void)hideCheckStatus
{
    _statusLabel.hidden = YES;
    _statusLabel.frame = NSMakeRect(0, 32, 360, 18);
    _statusLabel.textColor = [NSColor secondaryLabelColor];
}

- (void)showCheckError:(NSString *)text
{
    // Two lines' worth of room: the sentence has to name a way out, and it can
    // take the progress bar's space since the bar is gone by then.
    _statusLabel.frame = NSMakeRect(0, 20, 360, 32);
    _statusLabel.textColor = [NSColor systemRedColor];
    _statusLabel.stringValue = text;
    _statusLabel.hidden = NO;
}

- (void)setCredentialFieldsEnabled:(BOOL)enabled
{
    _userField.enabled = enabled;
    _passPlain.enabled = enabled;
    _passSecure.enabled = enabled;
}

// Basic-Auth GET on Sunshine's web-UI port with the credentials as typed. The
// pane waits on the answer (spinner running, fields frozen) instead of walking
// the user into a pairing that would fail minutes later, with nothing on screen
// to explain it. Asynchronous on purpose: blocking shouldExitPane: would freeze
// Installer.app and its progress bar with it.
- (void)startCredentialCheck
{
    NSString *user = [_userField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *pass = [self passwordValue];

    _checking = YES;
    [self setCredentialFieldsEnabled:NO];
    [self hideCheckStatus];
    _statusLabel.stringValue = @"Checking the Sunshine credentials…";
    _statusLabel.hidden = NO;
    _progress.indeterminate = YES;
    _progress.hidden = NO;
    [_progress startAnimation:nil];

    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = 8.0;
    cfg.timeoutIntervalForResource = 12.0;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg delegate:self
                                                    delegateQueue:nil];

    NSMutableURLRequest *req = [NSMutableURLRequest
        requestWithURL:[NSURL URLWithString:@"https://127.0.0.1:47990/api/apps"]];
    NSString *basic = [[[NSString stringWithFormat:@"%@:%@", user, pass]
        dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0];
    [req setValue:[@"Basic " stringByAppendingString:basic] forHTTPHeaderField:@"Authorization"];

    _checkTask = [session
        dataTaskWithRequest:req
          completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
              // 0 = never answered. Any real status means the header got past
              // the guard, so only 401/403 can mean "wrong credentials" — a
              // Sunshine whose API differs is never taken for a bad password.
              NSInteger status =
                  (!error && [response isKindOfClass:[NSHTTPURLResponse class]])
                      ? ((NSHTTPURLResponse *)response).statusCode
                      : 0;
              // A delegate session holds its delegate — this one has done its
              // one job, so let it go rather than pin the pane forever.
              [session finishTasksAndInvalidate];
              dispatch_async(dispatch_get_main_queue(), ^{
                  [self finishCredentialCheck:status];
              });
          }];
    [_checkTask resume];
}

- (void)finishCredentialCheck:(NSInteger)status
{
    if (!_checking) return; // Skip won the race
    _checking = NO;
    _checkTask = nil;
    [_progress stopAnimation:nil];
    _progress.indeterminate = NO;
    _progress.hidden = YES;
    [self setCredentialFieldsEnabled:YES];

    if (status == 401 || status == 403) {
        [self showCheckError:@"Sunshine refused these credentials. Check them in its web "
                             @"interface (https://localhost:47990), or click Skip."];
        return;
    }
    if (status == 0) {
        [self showCheckError:@"Sunshine is installed but is not answering. Start it and try "
                             @"again, or click Skip to pair later."];
        return;
    }

    _credsVerified = YES;
    [self hideCheckStatus];
    // Same rule as Skip: hand off before leaving, gotoNextPane is not guaranteed
    // to route through shouldExitPane:.
    [self writeHandoff];
    [self gotoNextPane];
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

// Sunshine self-signs the certificate of its loopback web UI, so the credential
// check has to accept it — for 127.0.0.1 and nothing else. Every other challenge
// (the DMG download's real TLS, the 401 the check is looking for) falls through
// to the default handling.
- (void)URLSession:(NSURLSession *)session
                   task:(NSURLSessionTask *)task
    didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
      completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition,
                                  NSURLCredential *))completionHandler
{
    NSURLProtectionSpace *space = challenge.protectionSpace;
    if ([space.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust] &&
        [space.host isEqualToString:@"127.0.0.1"] && space.serverTrust != NULL) {
        completionHandler(NSURLSessionAuthChallengeUseCredential,
                          [NSURLCredential credentialForTrust:space.serverTrust]);
        return;
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error
{
    // The credential check carries its own completion handler and owns the
    // status label while it runs — this one only speaks for the DMG download.
    if (task == _checkTask) return;
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
    // Skipping is exactly the way out of a credential check that will not pass.
    _checking = NO;
    [_checkTask cancel];
    _checkTask = nil;
    [_progress stopAnimation:nil];
    _progress.hidden = YES;
    [self setCredentialFieldsEnabled:YES];
    [self hideCheckStatus];
    _statusLabel.hidden = NO;
    _statusLabel.stringValue = @"Sunshine setup skipped.";
    // Write it now: gotoNextPane is not guaranteed to route through
    // shouldExitPane:, and the hand-off must never be left half-written.
    [self writeHandoff];
    [self gotoNextPane];
}

// Credentials are only mandatory when they will actually be used: a fresh
// install applies them via `sunshine --creds`, an existing install needs its
// real ones to accept the PIN. Skip and the already-paired case need neither.
//
// For an existing install the pane also refuses to advance until Sunshine itself
// has accepted the pair: the user is the only one who knows those credentials,
// and someone who does not is better told here — where Skip is right there —
// than left with a pairing that quietly failed.
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
        if (_case == MWSunshineUnpaired && !_credsVerified) {
            // A check already in flight: let it finish — it advances the pane
            // itself once Sunshine accepts.
            if (!_checking) [self startCredentialCheck];
            return NO;
        }
    }

    [self writeHandoff];
    return YES;
}

@end
