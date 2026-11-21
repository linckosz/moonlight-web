/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "test_framework.h"
#include "input/HidDescriptor.h"
#include "streaming/HidCollectionsJson.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace mw::native::input::hid;

namespace {

// The G923's game interface as Chrome 154 showed it on Windows (H0 survey,
// 03/10): the joystick's report 1, then one HID++ collection, with the bounds
// Chrome gives there (buttons 0..0, vendor bytes 255..0).
const char* kG923 = R"([
  {"usagePage": 1, "usage": 4, "type": 1, "children": [], "outputReports": [], "featureReports": [],
   "inputReports": [{"reportId": 1, "items": [
     {"usages": [65593], "reportSize": 4, "reportCount": 1, "logicalMinimum": 0, "logicalMaximum": 7,
      "physicalMinimum": 0, "physicalMaximum": 315, "hasNull": true, "unitSystem": "english-rotation",
      "unitFactorLengthExponent": 1, "unitExponent": 0},
     {"isRange": true, "usageMinimum": 589825, "usageMaximum": 589847, "usages": [], "reportSize": 1,
      "reportCount": 23, "logicalMinimum": 0, "logicalMaximum": 0},
     {"isConstant": true, "isArray": true, "reportSize": 5, "reportCount": 1},
     {"usages": [65584], "reportSize": 16, "reportCount": 1, "logicalMinimum": 0, "logicalMaximum": 65535},
     {"usages": [65585], "reportSize": 8, "reportCount": 1, "logicalMinimum": 0, "logicalMaximum": 255},
     {"usages": [65586], "reportSize": 8, "reportCount": 1, "logicalMinimum": 0, "logicalMaximum": 255},
     {"usages": [65589], "reportSize": 8, "reportCount": 1, "logicalMinimum": 0, "logicalMaximum": 255},
     {"isRange": true, "usageMinimum": 4278779905, "usageMaximum": 4278779907, "reportSize": 1,
      "reportCount": 3, "logicalMinimum": 0, "logicalMaximum": 0},
     {"isConstant": true, "isArray": true, "reportSize": 5, "reportCount": 1}]}]},
  {"usagePage": 65347, "usage": 1538, "type": 1, "children": [], "featureReports": [],
   "inputReports": [{"reportId": 17, "items": [{"isArray": true, "usages": [4282581000], "reportSize": 8,
      "reportCount": 19, "logicalMinimum": 255, "logicalMaximum": 0}]}],
   "outputReports": [{"reportId": 17, "items": [{"isArray": true, "usages": [4282581000], "reportSize": 8,
      "reportCount": 19, "logicalMinimum": 255, "logicalMaximum": 0}]}]}
])";

QJsonArray array(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).array();
}

} // namespace

void run_hid_collections_json_tests()
{
    SECTION("HidCollectionsJson — the G923 as the page sends it becomes an accepted descriptor");
    {
        QString error;
        std::vector<Collection> c = collectionsFromJson(array(kG923), &error);
        CHECK(error.isEmpty());
        CHECK_EQ(c.size(), static_cast<size_t>(2));
        CHECK_EQ(c[0].inputReports[0].items.size(), static_cast<size_t>(9));
        const ReportItem& hat = c[0].inputReports[0].items[0];
        CHECK(hat.hasNull && hat.unitSystem == UnitSystem::EnglishRotation &&
              hat.unitFactors[0] == 1);
        CHECK_EQ(c[1].inputReports[0].items[0].usages[0], 4282581000u);
        repairBounds(c);
        const std::vector<uint8_t> d = encode(c);
        CHECK(validate(d).empty());
        const Parsed p = parse(d);
        CHECK_EQ(reportBytes(p, Kind::Input, 1), static_cast<size_t>(11));
        CHECK_EQ(reportBytes(p, Kind::Output, 17), static_cast<size_t>(20));
    }

    SECTION("HidCollectionsJson — what cannot be read is said, never guessed");
    {
        QString error;
        collectionsFromJson(
            array(R"([{"usagePage": 1, "usage": 4, "inputReports": [{"reportId": 300}]}])"),
            &error);
        CHECK(!error.isEmpty());
        error.clear();
        collectionsFromJson(
            array(
                R"([{"usagePage": 1, "inputReports": [{"reportId": 1, "items": [{"usages": [-1]}]}]}])"),
            &error);
        CHECK(!error.isEmpty());
        error.clear();
        collectionsFromJson(array(R"([5])"), &error);
        CHECK(!error.isEmpty());
        // Nested deeper than a device ever does: refused before it costs anything.
        QByteArray deep = "[";
        for (int i = 0; i < 40; ++i)
            deep += R"({"usagePage": 1, "children": [)";
        for (int i = 0; i < 40; ++i)
            deep += "]}";
        deep += "]";
        error.clear();
        collectionsFromJson(QJsonDocument::fromJson(deep).array(), &error);
        CHECK(!error.isEmpty());
        // Missing fields take WebHID's defaults.
        error.clear();
        std::vector<Collection> c =
            collectionsFromJson(array(R"([{"usagePage": 2, "usage": 197}])"), &error);
        CHECK(error.isEmpty() && c.size() == 1 && c[0].type == Application &&
              c[0].inputReports.empty());
    }

    SECTION("HidCollectionsJson — a device's name is kept clean and short");
    {
        const auto nameOf = [](const QString& productName) {
            return deviceNameFromJson(QJsonObject{{"productName", productName}});
        };
        CHECK_EQ(nameOf(QStringLiteral("G923 Racing Wheel for PlayStation and PC")),
                 std::string("G923 Racing Wheel for PlayStation and PC"));
        // Control, format, line and paragraph characters are dropped, the
        // rest kept as it is, accents and all.
        CHECK_EQ(nameOf(QStringLiteral("Pad\nTwo\r\tThree")), std::string("PadTwoThree"));
        CHECK_EQ(nameOf(QString(QChar(0)) + QStringLiteral("Pad")), std::string("Pad"));
        CHECK_EQ(nameOf(QStringLiteral("Pad") + QChar(0x2028) + QStringLiteral("Two") +
                        QChar(0x2029) + QChar(0x200E) + QChar(0x202E) + QChar(0xFEFF)),
                 std::string("PadTwo"));
        CHECK_EQ(nameOf(QStringLiteral("Manette sans fil é")),
                 QStringLiteral("Manette sans fil é").toStdString());
        // At most kMaxDeviceNameChars characters, counted after the cleaning.
        const std::string longName = nameOf(QString(200, QChar('x')));
        CHECK_EQ(static_cast<int>(longName.size()), kMaxDeviceNameChars);
        CHECK_EQ(
            static_cast<int>(nameOf(QString(150, QChar('\n')) + QString(130, QChar('y'))).size()),
            kMaxDeviceNameChars);
        // None, the wrong type, or nothing left: empty.
        CHECK(deviceNameFromJson(QJsonObject{}).empty());
        CHECK(deviceNameFromJson(QJsonObject{{"productName", 42}}).empty());
        CHECK(nameOf(QStringLiteral("\n\r\t")).empty());
    }
}
