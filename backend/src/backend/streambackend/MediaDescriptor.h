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

#pragma once

#include <QByteArray>
#include <QString>

// What a stream backend hands back once an app is running: everything the
// media engine needs to attach to that session, and nothing about *how* the
// backend got there.
//
// This is a tagged union. The tag is what lets a new media source land without
// touching IStreamBackend or any provider. StreamSession switches on `type` in
// a single place; see the media-engine branch point in streaming/Session.cpp.
enum class MediaType
{
    GameStreamRtsp,

    /// This machine's own screen, captured and encoded in-process by
    /// backend/native-host/. There is no host to dial, no session to negotiate
    /// and no key to exchange — which is exactly the point: everything a remote
    /// host needs (RTSP, RTP, FEC, a second layer of AES) is what the native
    /// engine exists to remove.
    NativeHost,
};

// Valid when MediaDescriptor::type == MediaType::GameStreamRtsp.
// Mirrors what MoonlightShim::InitParams needs; the backend fills it from the
// /launch (or /resume) response plus the host it talked to.
struct GameStreamMedia
{
    QString rtspSessionUrl; // NvHTTP::parseSessionUrl() of the launch XML
    QString hostAddress;    // address moonlight-common-c should dial
    QString appVersion;     // host-reported, drives protocol quirks
    QString gfeVersion;
    int serverCodecModeSupport = 0; // host codec capability bitmap
    QByteArray aesKey;              // rikey — 16 random bytes
    int rikeyid = 0;                // rikeyid — IV prefix
};

// Valid when MediaDescriptor::type == MediaType::NativeHost.
//
// Strikingly smaller than GameStreamMedia, and that is the whole story: no
// address, no session URL, no version quirks, no AES key. What is left is what
// the user actually chose (a display) plus what the browser can decode.
struct NativeHostMedia
{
    /// Which display to capture — DisplayInfo::id from the native engine's
    /// probe. This is the one and only technical choice a user makes.
    int displayId = -1;

    /// Codecs the browser accepts, best first, as a VIDEO_FORMAT_* bitmask.
    /// The engine intersects it with what the display's GPU can encode; no
    /// codec question is ever put to the user.
    int clientVideoFormats = 0;

    /// True when the client asked for HDR. Honoured only if the display is
    /// really in an HDR mode and the encoder has a 10-bit path — otherwise the
    /// session runs SDR and says so, rather than failing.
    bool hdrRequested = false;
};

struct MediaDescriptor
{
    MediaType type = MediaType::GameStreamRtsp;
    GameStreamMedia gameStream;
    NativeHostMedia nativeHost;
};
