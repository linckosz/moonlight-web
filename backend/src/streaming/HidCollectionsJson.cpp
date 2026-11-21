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

#include "HidCollectionsJson.h"

#include <QJsonObject>

#include <cmath>

using namespace mw::native::input::hid;

namespace {

constexpr int kMaxDepth = 16;

bool boolOf(const QJsonObject& o, const char* key, bool fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : fallback;
}

double numberOf(const QJsonObject& o, const char* key, double fallback = 0)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

/// An integer in [lo, hi], or nothing: a page sends doubles, and a usage or a
/// bound that is not a whole number in range is not one to guess at.
bool intIn(double v, double lo, double hi, int64_t* out)
{
    if (!std::isfinite(v) || v != std::floor(v) || v < lo || v > hi) return false;
    *out = static_cast<int64_t>(v);
    return true;
}

UnitSystem unitSystemOf(const QString& s)
{
    if (s == QLatin1String("si-linear")) return UnitSystem::SiLinear;
    if (s == QLatin1String("si-rotation")) return UnitSystem::SiRotation;
    if (s == QLatin1String("english-linear")) return UnitSystem::EnglishLinear;
    if (s == QLatin1String("english-rotation")) return UnitSystem::EnglishRotation;
    if (s == QLatin1String("vendor-defined")) return UnitSystem::Vendor;
    return UnitSystem::None;
}

class Reader
{
public:
    QString error;

    std::vector<Collection> collections(const QJsonArray& a, int depth)
    {
        std::vector<Collection> out;
        if (depth > kMaxDepth) {
            fail(QStringLiteral("collections nested deeper than %1").arg(kMaxDepth));
            return out;
        }
        for (const QJsonValue& v : a) {
            if (!v.isObject()) {
                fail(QStringLiteral("a collection is not an object"));
                continue;
            }
            out.push_back(collection(v.toObject(), depth));
        }
        return out;
    }

private:
    void fail(const QString& why)
    {
        if (error.isEmpty()) error = why;
    }

    Collection collection(const QJsonObject& o, int depth)
    {
        Collection c;
        int64_t v = 0;
        if (intIn(numberOf(o, "usagePage"), 0, 0xFFFF, &v)) c.usagePage = static_cast<uint16_t>(v);
        if (intIn(numberOf(o, "usage"), 0, 0xFFFF, &v)) c.usage = static_cast<uint16_t>(v);
        if (intIn(numberOf(o, "type", Application), 0, 0xFF, &v)) c.type = static_cast<uint8_t>(v);
        c.inputReports = reports(o.value(QLatin1String("inputReports")).toArray());
        c.outputReports = reports(o.value(QLatin1String("outputReports")).toArray());
        c.featureReports = reports(o.value(QLatin1String("featureReports")).toArray());
        c.children = collections(o.value(QLatin1String("children")).toArray(), depth + 1);
        return c;
    }

    std::vector<Report> reports(const QJsonArray& a)
    {
        std::vector<Report> out;
        for (const QJsonValue& v : a) {
            const QJsonObject o = v.toObject();
            Report r;
            int64_t id = 0;
            if (!intIn(numberOf(o, "reportId"), 0, 0xFF, &id)) {
                fail(QStringLiteral("report id out of range"));
                continue;
            }
            r.reportId = static_cast<uint8_t>(id);
            for (const QJsonValue& it : o.value(QLatin1String("items")).toArray())
                r.items.push_back(item(it.toObject()));
            out.push_back(std::move(r));
        }
        return out;
    }

    ReportItem item(const QJsonObject& o)
    {
        ReportItem it;
        it.isAbsolute = boolOf(o, "isAbsolute", true);
        it.isArray = boolOf(o, "isArray", false);
        it.isBufferedBytes = boolOf(o, "isBufferedBytes", false);
        it.isConstant = boolOf(o, "isConstant", false);
        it.isLinear = boolOf(o, "isLinear", true);
        it.isRange = boolOf(o, "isRange", false);
        it.isVolatile = boolOf(o, "isVolatile", false);
        it.hasNull = boolOf(o, "hasNull", false);
        it.hasPreferredState = boolOf(o, "hasPreferredState", true);
        it.wrap = boolOf(o, "wrap", false);

        int64_t v = 0;
        for (const QJsonValue& u : o.value(QLatin1String("usages")).toArray()) {
            if (intIn(u.toDouble(-1), 0, 0xFFFFFFFFu, &v))
                it.usages.push_back(static_cast<uint32_t>(v));
            else
                fail(QStringLiteral("usage out of range"));
        }
        if (intIn(numberOf(o, "usageMinimum"), 0, 0xFFFFFFFFu, &v))
            it.usageMinimum = static_cast<uint32_t>(v);
        if (intIn(numberOf(o, "usageMaximum"), 0, 0xFFFFFFFFu, &v))
            it.usageMaximum = static_cast<uint32_t>(v);
        if (intIn(numberOf(o, "reportSize"), 0, 0xFFFF, &v))
            it.reportSize = static_cast<uint16_t>(v);
        if (intIn(numberOf(o, "reportCount"), 0, 0xFFFF, &v))
            it.reportCount = static_cast<uint16_t>(v);
        if (intIn(numberOf(o, "unitExponent"), -8, 7, &v)) it.unitExponent = static_cast<int8_t>(v);
        it.unitSystem = unitSystemOf(o.value(QLatin1String("unitSystem")).toString());
        static const char* const kFactors[6] = {
            "unitFactorLengthExponent",  "unitFactorMassExponent",
            "unitFactorTimeExponent",    "unitFactorTemperatureExponent",
            "unitFactorCurrentExponent", "unitFactorLuminousIntensityExponent",
        };
        for (int i = 0; i < 6; ++i)
            if (intIn(numberOf(o, kFactors[i]), -8, 7, &v))
                it.unitFactors[i] = static_cast<int8_t>(v);

        const double lo = -2147483648.0, hi = 2147483647.0;
        if (intIn(numberOf(o, "logicalMinimum"), lo, hi, &v))
            it.logicalMinimum = static_cast<int32_t>(v);
        if (intIn(numberOf(o, "logicalMaximum"), lo, hi, &v))
            it.logicalMaximum = static_cast<int32_t>(v);
        if (intIn(numberOf(o, "physicalMinimum"), lo, hi, &v))
            it.physicalMinimum = static_cast<int32_t>(v);
        if (intIn(numberOf(o, "physicalMaximum"), lo, hi, &v))
            it.physicalMaximum = static_cast<int32_t>(v);
        return it;
    }
};

} // namespace

std::vector<Collection> collectionsFromJson(const QJsonArray& array, QString* error)
{
    Reader r;
    std::vector<Collection> out = r.collections(array, 0);
    if (error) *error = r.error;
    return out;
}

std::string deviceNameFromJson(const QJsonObject& message)
{
    QString name = message.value(QStringLiteral("productName")).toString();
    name.removeIf([](QChar c) {
        const QChar::Category cat = c.category();
        return cat == QChar::Other_Control || cat == QChar::Other_Format ||
               cat == QChar::Separator_Line || cat == QChar::Separator_Paragraph;
    });
    return name.left(kMaxDeviceNameChars).toStdString();
}
