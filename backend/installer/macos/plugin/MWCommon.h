/*
 * MoonlightWeb — macOS Installer.app plugin, shared helpers.
 *
 * Two chained InstallerPanes collect the same choices the Windows Inno installer
 * does, then hand them to the .pkg postinstall (which runs as root) through a
 * plist in /tmp: username, password, internet flag, and the path of the Sunshine
 * DMG the download pane fetched.
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

// Merge key/values into the hand-off plist (created if absent), preserving any
// keys a previous pane already wrote.
static inline void MWHandoffMerge(NSDictionary *values)
{
    NSMutableDictionary *d =
        [NSMutableDictionary dictionaryWithContentsOfFile:kMWHandoffPath] ?: [NSMutableDictionary dictionary];
    [d addEntriesFromDictionary:values];
    [d writeToFile:kMWHandoffPath atomically:YES];
}
