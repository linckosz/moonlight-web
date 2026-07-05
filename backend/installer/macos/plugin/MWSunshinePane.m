/*
 * MoonlightWeb — Installer.app plugin: single "Sunshine" pane.
 *
 * Collects the same choices the Windows Inno installer does — Sunshine username/
 * password + "allow the public Internet link" — and, in the background, downloads
 * the Sunshine DMG with a real progress bar (the privileged copy to /Applications
 * happens in the .pkg postinstall, which runs as root). Everything is handed to
 * the postinstall through the /tmp plist (see MWCommon.h).
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

@interface MWSunshinePane () <NSURLSessionDownloadDelegate>
@end

@implementation MWSunshinePane {
    BOOL _built;
    NSTextField *_userField;
    NSSecureTextField *_passField;
    NSButton *_internetCheck;
    NSTextField *_statusLabel;
    NSProgressIndicator *_progress;
    NSURLSessionDownloadTask *_task;
    BOOL _started;
}

// Sidebar label comes from InstallerSectionTitle; this is the pane title shown
// above the content area.
- (NSString *)title { return @"Sunshine"; }

- (NSTextField *)labelAt:(CGFloat)y text:(NSString *)s in:(NSView *)view
{
    NSTextField *l = [[NSTextField alloc] initWithFrame:NSMakeRect(0, y, 470, 18)];
    l.stringValue = s;
    l.bezeled = NO; l.drawsBackground = NO; l.editable = NO; l.selectable = NO;
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

    [self labelAt:212 text:@"Enter the Sunshine credentials MoonlightWeb should use to pair."
               in:view];

    [self labelAt:184 text:@"Username" in:view];
    _userField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 160, 220, 22)];
    _userField.stringValue = @"admin";
    [view addSubview:_userField];

    [self labelAt:128 text:@"Password" in:view];
    _passField = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 104, 220, 22)];
    [view addSubview:_passField];

    _internetCheck = [[NSButton alloc] initWithFrame:NSMakeRect(0, 66, 470, 22)];
    [_internetCheck setButtonType:NSButtonTypeSwitch];
    _internetCheck.title = @"Allow a secure public Internet link (recommended)";
    _internetCheck.state = NSControlStateValueOn;
    [view addSubview:_internetCheck];

    _statusLabel = [self labelAt:34 text:@"Preparing Sunshine…" in:view];
    _statusLabel.textColor = [NSColor secondaryLabelColor];

    _progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 12, 470, 16)];
    _progress.style = NSProgressIndicatorStyleBar;
    _progress.indeterminate = NO;
    _progress.minValue = 0.0;
    _progress.maxValue = 1.0;
    [view addSubview:_progress];
}

// Start the Sunshine download once, when the pane first appears. It runs while
// the user fills in the credentials; the progress bar reflects it.
- (void)didEnterPane:(InstallerSectionDirection)dir
{
    if (_started) return;
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
        self->_statusLabel.stringValue = @"Sunshine will be downloaded during installation.";
    });
}

#pragma mark - Navigation

// Require credentials before leaving forward; the download may still be running
// (the postinstall re-fetches the DMG if it isn't ready).
- (BOOL)shouldExitPane:(InstallerSectionDirection)dir
{
    if (dir != InstallerDirectionForward) return YES;

    NSString *user = [_userField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *pass = _passField.stringValue;
    if (user.length == 0 || pass.length == 0) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"Sunshine credentials required";
        a.informativeText = @"Please enter a username and password so MoonlightWeb can pair "
                            @"with Sunshine automatically.";
        [a runModal];
        return NO;
    }

    MWHandoffMerge(@{
        @"username" : user,
        @"password" : pass,
        @"internet" : @(_internetCheck.state == NSControlStateValueOn),
    });
    return YES;
}

@end
