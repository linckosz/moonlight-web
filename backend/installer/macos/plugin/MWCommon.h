/*
 * MoonlightWeb — macOS Installer.app plugin, shared helpers.
 *
 * The single "Internet" InstallerPane asks the one question the Windows Inno
 * installer asks, then hands the answer to the .pkg postinstall (which runs as
 * root) through a plist in /tmp: the internet flag and the exact wording it was
 * given for.
 *
 * It used to carry a Sunshine username and password through that same file, in
 * plaintext, plus the path of a DMG the pane had downloaded. None of it exists
 * any more: the app captures and encodes this Mac itself (design §20).
 */
#import <Cocoa/Cocoa.h>

// Hand-off plist read by scripts/postinstall via `defaults read`. The extension
// is added by NSDictionary/`defaults`; keep the base path in sync with the
// script's HANDOFF variable.
static NSString *const kMWHandoffPath = @"/tmp/moonlightweb-provisioning.plist";

// The Internet opt-in agreement, in the two blocks it is TYPESET as — the
// checkbox's own line, then the explanation under it — and, joined by a single
// space, the one string handed to the server as the consent text of its
// versioned consent record.
//
// Split for legibility, not for meaning: a checkbox whose title ran to six
// wrapped lines was the shape that shipped first, and it read badly. The web
// wizard already lays the same agreement out this way (body, then the line the
// user ticks). What is recorded stays byte for byte what is displayed, which is
// the whole point of having one source of truth here.
//
// It must name every party that learns something — the router, the peer, the
// introduction server, the STUN server — because what is recorded has to be
// what was read.
static inline NSString *MWInternetConsentLead(void)
{
    return @"Allow the Internet link (recommended).";
}

static inline NSString *MWInternetConsentBody(void)
{
    return @"The router is asked (UPnP) to open one "
           @"UDP port per session; it carries nothing but the encrypted stream, and every "
           @"connection on it authenticates first. A rendezvous server introduces the two "
           @"sides, so this Mac's public IP is seen only by that server and by whoever holds "
           @"the link; a MoonlightWeb STUN server is asked what that address is. No DNS "
           @"record, no certificate, ports 80/443 stay closed.";
}

static inline NSString *MWInternetConsentText(void)
{
    return [NSString stringWithFormat:@"%@ %@", MWInternetConsentLead(), MWInternetConsentBody()];
}

// True when a prior install already authorized the public Internet link: the
// server persists internet_access_enabled in its settings.json, so a re-install
// pre-ticks the opt-in instead of silently forgetting it. Qt's
// QStandardPaths::AppDataLocation → ~/Library/Application Support/<org>/<app>,
// both "MoonlightWeb". Absent file / key → NO (first install stays unchecked).
static inline BOOL MWInternetAlreadyAuthorized(void)
{
    NSArray<NSString *> *base =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (base.count == 0)
        return NO;
    NSString *path =
        [base.firstObject stringByAppendingPathComponent:@"MoonlightWeb/MoonlightWeb/settings.json"];
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data)
        return NO;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:[NSDictionary class]])
        return NO;
    return [[(NSDictionary *)json objectForKey:@"internet_access_enabled"] boolValue];
}

// Merge key/values into the hand-off plist (created if absent), preserving keys
// written earlier. 0600 out of habit rather than need now that no credential
// travels through it: the postinstall (root) reads and deletes it.
static inline void MWHandoffMerge(NSDictionary *values)
{
    NSMutableDictionary *d =
        [NSMutableDictionary dictionaryWithContentsOfFile:kMWHandoffPath] ?: [NSMutableDictionary dictionary];
    [d addEntriesFromDictionary:values];
    [d writeToFile:kMWHandoffPath atomically:YES];
    [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions : @0600}
                                     ofItemAtPath:kMWHandoffPath
                                            error:nil];
}
