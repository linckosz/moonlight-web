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

#include "mw/native/HidPassthrough.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <string>
#include <vector>

/// WebHID's `collections`, as a page serialises them (field names of
/// HIDCollectionInfo and HIDReportItem, hid-probe.html's plainCollection), read
/// into the encoder's structures. Missing fields take WebHID's defaults; a
/// value of the wrong type reads as missing. `error` says what could not be
/// read at all (not an array, a usage out of range).
std::vector<mw::native::input::hid::Collection> collectionsFromJson(const QJsonArray& array,
                                                                    QString* error);

/// The device's name, as the page sends it (`productName`, WebHID's), in
/// UTF-8: no control, format, line or paragraph separator character (the
/// server's rule for a browser's words, main.cpp's logSafe), at most
/// kMaxDeviceNameChars characters. Empty when there is none, or none of it
/// is kept.
constexpr int kMaxDeviceNameChars = 120;
std::string deviceNameFromJson(const QJsonObject& message);
