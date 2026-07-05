/*
 * Minimal re-declaration of the InstallerPlugins framework's InstallerPane.
 *
 * InstallerPlugins.framework is deprecated: recent macOS SDKs still ship the
 * framework *binary* (so `-framework InstallerPlugins` links and Installer.app
 * loads our pane at runtime) but no longer ship its public headers, so
 * `#import <InstallerPlugins/InstallerPlugins.h>` fails to compile. We declare
 * only the small, long-stable API our pane overrides/uses; the real class from
 * the linked framework provides the implementation (including the contentView
 * IBOutlet the nib connects — see MWSunshinePane.xib).
 */
#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, InstallerSectionDirection) {
    InstallerDirectionForward = 0,
    InstallerDirectionBackward,
    InstallerDirectionUndefined
};

@class InstallerSection;

@interface InstallerPane : NSObject
- (instancetype)initWithSection:(InstallerSection *)parent;
- (InstallerSection *)section;
- (NSView *)contentView;
- (void)setContentView:(NSView *)view;
- (NSString *)title;
- (void)setNextEnabled:(BOOL)enabled;
- (BOOL)shouldExitPane:(InstallerSectionDirection)dir;
- (void)willEnterPane:(InstallerSectionDirection)dir;
- (void)didEnterPane:(InstallerSectionDirection)dir;
@end
