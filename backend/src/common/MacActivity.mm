/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "MacActivity.h"

#import <Foundation/Foundation.h>

#include <QDebug>
#include <mutex>

namespace {
std::mutex s_Mutex;
int s_RefCount = 0;
// Token returned by beginActivityWithOptions; retained manually (MRC build).
id<NSObject> s_Activity = nil;
} // namespace

namespace MacActivity {

void beginStreaming()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (s_RefCount++ > 0) return;
    // Callers may be on Qt worker threads that have no autorelease pool; the
    // pool drains the autoreleased token reference, our retain keeps it alive.
    @autoreleasepool {
        s_Activity = [[[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                              reason:@"Streaming session active"] retain];
    }
    qInfo() << "[MacActivity] App Nap suppression ON (streaming)";
}

void endStreaming()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (s_RefCount == 0 || --s_RefCount > 0) return;
    if (s_Activity != nil) {
        [[NSProcessInfo processInfo] endActivity:s_Activity];
        [s_Activity release];
        s_Activity = nil;
    }
    qInfo() << "[MacActivity] App Nap suppression OFF";
}

} // namespace MacActivity
