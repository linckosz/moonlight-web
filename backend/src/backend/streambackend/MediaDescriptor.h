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
// This is a tagged union. Today there is exactly one transport (GameStream
// RTSP, driven by moonlight-common-c through MoonlightShim), but the tag is
// what lets a future transport — e.g. punktfunk's QUIC protocol — land without
// touching IStreamBackend or any provider. StreamSession switches on `type` in
// a single place; see the media-engine branch point in streaming/Session.cpp.
enum class MediaType
{
    GameStreamRtsp,
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

struct MediaDescriptor
{
    MediaType type = MediaType::GameStreamRtsp;
    GameStreamMedia gameStream;
};
