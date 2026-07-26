/*
 * MoonlightWeb — macOS Installer.app plugin, shared helpers.
 *
 * The single "Sunshine" InstallerPane collects the same choices the Windows Inno
 * installer does, then hands them to the .pkg postinstall (which runs as root)
 * through a plist in /tmp: username, password, internet flag, and the path of
 * the Sunshine DMG the pane downloaded in the background.
 */
#import <Cocoa/Cocoa.h>

// Hand-off plist read by scripts/postinstall via `defaults read`. The extension
// is added by NSDictionary/`defaults`; keep the base path in sync with the
// script's HANDOFF variable.
static NSString *const kMWHandoffPath = @"/tmp/moonlightweb-provisioning.plist";

// Sunshine release asset for the running CPU (arm64 pkg today; future-proofed).
static inline NSString *MWSunshineArch(void)
{
#if defined(__x86_64__)
    return @"x86_64";
#else
    return @"arm64";
#endif
}

static inline NSString *MWSunshineDmgURL(void)
{
    return [NSString stringWithFormat:@"https://github.com/LizardByte/Sunshine/releases/latest/"
                                      @"download/Sunshine-macOS-%@.dmg",
                                      MWSunshineArch()];
}

// The Internet opt-in checkbox label. Single source of truth: displayed in the
// pane AND handed to the server as the consent text it records in its DNS
// registration audit log.
static inline NSString *MWInternetConsentText(void)
{
    return @"Allow a secure public Internet link (recommended)";
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

// Path of the Sunshine binary already installed on this machine, or nil. Same
// lookup order as the .pkg postinstall's sunshine_bin().
static inline NSString *MWSunshineInstalledPath(void)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *p in @[
             @"/Applications/Sunshine.app/Contents/MacOS/sunshine",
             @"/opt/homebrew/bin/sunshine",
             @"/usr/local/bin/sunshine",
         ]) {
        if ([fm isExecutableFileAtPath:p])
            return p;
    }
    return nil;
}

// This Mac's short host name ("MacBook-Pro"), the name Sunshine advertises by
// default and the one the server stores for a locally discovered host.
static inline NSString *MWLocalHostName(void)
{
    NSString *name = [[NSProcessInfo processInfo] hostName] ?: @"";
    if ([name hasSuffix:@".local"])
        name = [name substringToIndex:name.length - 6];
    return name;
}

// Does this settings dictionary hold a host entry that is (a) paired and (b) the
// Sunshine on THIS machine? Qt flattens/nests its persisted host array
// differently per platform, so match on the key SUFFIX and recurse: that covers
// both a flat "hosts.1.pairState" key and a nested hosts → 1 → pairState tree.
static inline BOOL MWDictHasPairedLocalHost(NSDictionary *d, NSString *localName)
{
    static NSString *const kPairState = @"pairState";
    for (NSString *key in d) {
        id value = d[key];
        if ([value isKindOfClass:[NSDictionary class]]) {
            if (MWDictHasPairedLocalHost((NSDictionary *)value, localName))
                return YES;
            continue;
        }
        if ([value isKindOfClass:[NSArray class]]) {
            for (id item in (NSArray *)value)
                if ([item isKindOfClass:[NSDictionary class]] &&
                    MWDictHasPairedLocalHost((NSDictionary *)item, localName))
                    return YES;
            continue;
        }
        if (![key hasSuffix:kPairState] || ![value isKindOfClass:[NSString class]] ||
            ![(NSString *)value isEqualToString:@"paired"])
            continue;

        // Sibling keys share this entry's prefix ("" when nested, "hosts.1."
        // when flattened).
        NSString *prefix = [key substringToIndex:key.length - kPairState.length];
        id manual = d[[prefix stringByAppendingString:@"manualaddress"]];
        if ([manual isKindOfClass:[NSString class]] &&
            ([manual isEqualToString:@"127.0.0.1"] || [manual isEqualToString:@"::1"] ||
             [[manual lowercaseString] isEqualToString:@"localhost"]))
            return YES;
        id host = d[[prefix stringByAppendingString:@"hostname"]];
        if (localName.length > 0 && [host isKindOfClass:[NSString class]] &&
            [(NSString *)host caseInsensitiveCompare:localName] == NSOrderedSame)
            return YES;
    }
    return NO;
}

// True when a previous MoonlightWeb run already paired with the local Sunshine.
// The server persists its host list through QSettings (ComputerManager::
// saveHosts / NvComputer::serialize); on macOS that lands in a CFPreferences
// plist under ~/Library/Preferences whose name carries the application name —
// the exact domain Qt derives from the organization is an implementation detail,
// so match the file name instead of hardcoding it. Nothing readable → NO, which
// simply falls back to the normal "ask for credentials" pane.
static inline BOOL MWLocalSunshinePaired(void)
{
    NSArray<NSString *> *base =
        NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES);
    if (base.count == 0)
        return NO;
    NSString *prefs = [base.firstObject stringByAppendingPathComponent:@"Preferences"];
    NSArray<NSString *> *entries =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:prefs error:nil];
    NSString *localName = MWLocalHostName();

    for (NSString *entry in entries) {
        if (![entry.pathExtension isEqualToString:@"plist"] ||
            [entry rangeOfString:@"MoonlightWeb" options:NSCaseInsensitiveSearch].location ==
                NSNotFound)
            continue;
        NSDictionary *d = [NSDictionary
            dictionaryWithContentsOfFile:[prefs stringByAppendingPathComponent:entry]];
        if (d && MWDictHasPairedLocalHost(d, localName))
            return YES;
    }
    return NO;
}

// Merge key/values into the hand-off plist (created if absent), preserving keys
// written earlier. 0600: it carries the Sunshine password in plaintext until the
// postinstall (root) reads and deletes it.
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
