/* MoonlightWeb - clipboard support. GPLv3 - see repository LICENSE. */
#include "ClipboardMac.h"

#import <AppKit/AppKit.h>

QString macClipboardText()
{
    @autoreleasepool {
        NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return text ? QString::fromUtf8([text UTF8String]) : QString();
    }
}

qint64 macClipboardChangeCount()
{
    @autoreleasepool {
        return [[NSPasteboard generalPasteboard] changeCount];
    }
}
