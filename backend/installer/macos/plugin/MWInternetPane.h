/*
 * MoonlightWeb — Installer.app plugin: single "Internet" pane. Instantiated
 * from MWInternetPane.nib, wired as the section's firstPane (see Info.plist).
 *
 * It used to be the "Sunshine" pane (credentials + DMG download + Internet
 * toggle); the app captures and encodes this Mac itself since September 2026
 * (design §20), so the only choice left to make here is the Internet link.
 */
#import "MWInstallerPane.h"

@interface MWInternetPane : InstallerPane
@end
