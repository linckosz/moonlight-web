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

#include "NativeCapabilitiesJson.h"

#include <QJsonArray>

using namespace mw::native;

namespace {

template <typename Enum> QJsonArray enumsToJson(const std::vector<Enum>& values)
{
    QJsonArray arr;
    for (Enum v : values)
        arr.append(static_cast<int>(v));
    return arr;
}

template <typename Enum> std::vector<Enum> enumsFromJson(const QJsonArray& arr)
{
    std::vector<Enum> out;
    out.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr)
        out.push_back(static_cast<Enum>(v.toInt()));
    return out;
}

QString u64(uint64_t v)
{
    // JSON numbers are doubles: a LUID would lose bits above 2^53.
    return QString::number(v, 16);
}

uint64_t u64(const QString& s)
{
    return s.toULongLong(nullptr, 16);
}

} // namespace

namespace NativeCapabilitiesJson {

QJsonObject toJson(const Capabilities& caps)
{
    QJsonObject obj;
    obj["schema"] = kSchema;
    obj["available"] = caps.available;
    obj["reason"] = static_cast<int>(caps.reason);
    obj["capture"] = static_cast<int>(caps.capture);
    obj["diagnostic"] = QString::fromStdString(caps.diagnostic);

    QJsonArray gpus;
    for (const GpuInfo& g : caps.gpus) {
        QJsonObject o;
        o["id"] = g.id;
        o["name"] = QString::fromStdString(g.name);
        o["vendorId"] = static_cast<int>(g.vendorId);
        o["deviceId"] = static_cast<int>(g.deviceId);
        o["nativeHandle"] = u64(g.nativeHandle);
        o["encoders"] = enumsToJson(g.encoders);
        o["codecs"] = enumsToJson(g.codecs);
        o["codecs444"] = enumsToJson(g.codecs444);
        o["supports10Bit"] = g.supports10Bit;
        gpus.append(o);
    }
    obj["gpus"] = gpus;

    QJsonArray displays;
    for (const DisplayInfo& d : caps.displays) {
        QJsonObject o;
        o["id"] = d.id;
        o["label"] = QString::fromStdString(d.label);
        o["detail"] = QString::fromStdString(d.detail);
        o["width"] = d.width;
        o["height"] = d.height;
        o["refreshMilliHz"] = d.refreshMilliHz;
        o["gpuId"] = d.gpuId;
        o["hdrActive"] = d.hdrActive;
        o["primary"] = d.primary;
        displays.append(o);
    }
    obj["displays"] = displays;
    return obj;
}

bool fromJson(const QJsonObject& obj, Capabilities& out)
{
    out = Capabilities{};
    out.available = false;
    out.reason = Unavailability::ProbeFailed;

    if (obj["schema"].toInt(-1) != kSchema) {
        out.diagnostic = "the console probe answered in an unknown format";
        return false;
    }

    out.available = obj["available"].toBool(false);
    out.reason = static_cast<Unavailability>(
        obj["reason"].toInt(static_cast<int>(Unavailability::ProbeFailed)));
    out.capture = static_cast<CaptureApi>(obj["capture"].toInt(0));
    out.diagnostic = obj["diagnostic"].toString().toStdString();

    for (const QJsonValue& v : obj["gpus"].toArray()) {
        const QJsonObject o = v.toObject();
        GpuInfo g;
        g.id = o["id"].toInt(-1);
        g.name = o["name"].toString().toStdString();
        g.vendorId = static_cast<uint32_t>(o["vendorId"].toInt());
        g.deviceId = static_cast<uint32_t>(o["deviceId"].toInt());
        g.nativeHandle = u64(o["nativeHandle"].toString());
        g.encoders = enumsFromJson<EncoderApi>(o["encoders"].toArray());
        g.codecs = enumsFromJson<Codec>(o["codecs"].toArray());
        g.codecs444 = enumsFromJson<Codec>(o["codecs444"].toArray());
        g.supports10Bit = o["supports10Bit"].toBool(false);
        out.gpus.push_back(std::move(g));
    }

    for (const QJsonValue& v : obj["displays"].toArray()) {
        const QJsonObject o = v.toObject();
        DisplayInfo d;
        d.id = o["id"].toInt(-1);
        d.label = o["label"].toString().toStdString();
        d.detail = o["detail"].toString().toStdString();
        d.width = o["width"].toInt();
        d.height = o["height"].toInt();
        d.refreshMilliHz = o["refreshMilliHz"].toInt();
        d.gpuId = o["gpuId"].toInt(-1);
        d.hdrActive = o["hdrActive"].toBool(false);
        d.primary = o["primary"].toBool(false);
        out.displays.push_back(std::move(d));
    }
    return true;
}

} // namespace NativeCapabilitiesJson
