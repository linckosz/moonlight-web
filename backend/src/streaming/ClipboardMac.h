/* MoonlightWeb - clipboard support. GPLv3 - see repository LICENSE. */
#pragma once

#include <QString>

// Main-thread only. Reads the actual pasteboard while the worker is inactive.
QString macClipboardText();

qint64 macClipboardChangeCount();
