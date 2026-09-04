/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The native engine's probe result, as it travels between the `--native-probe`
 * child in the console session and the service in session 0.
 *
 * Everything the host list and the launch decide on has to survive the trip
 * unchanged: which displays exist and their ids (the app ids the browser
 * launches), which GPU drives which display, which codecs each encoder can
 * produce and in which chroma, and a 64-bit adapter handle that a JSON number
 * would silently truncate. And a reply from a different build has to be
 * refused whole rather than half-trusted.
 */
#include "test_framework.h"

#include "../src/backend/streambackend/NativeCapabilitiesJson.h"

#include <QJsonDocument>

using namespace mw::native;

namespace {

Capabilities sample()
{
    Capabilities caps;
    caps.available = true;
    caps.reason = Unavailability::None;
    caps.capture = CaptureApi::DxgiDuplication;
    caps.diagnostic = "";

    GpuInfo nvidia;
    nvidia.id = 0;
    nvidia.name = "NVIDIA GeForce RTX 5060 Ti";
    nvidia.vendorId = 0x10DE;
    nvidia.deviceId = 0x2D04;
    nvidia.nativeHandle = 0xFFFFFFFF0000ABCDull; // above 2^53: a double would lose it
    nvidia.encoders = {EncoderApi::Nvenc};
    nvidia.codecs = {Codec::Av1, Codec::Hevc, Codec::H264};
    nvidia.codecs444 = {Codec::Hevc, Codec::H264};
    nvidia.supports10Bit = true;

    GpuInfo bare;
    bare.id = 1;
    bare.name = "Microsoft Basic Render Driver";
    bare.vendorId = 0x1414;

    caps.gpus = {nvidia, bare};

    DisplayInfo primary;
    primary.id = 0;
    primary.label = "Display 1";
    primary.detail = "M27Q \xE2\x80\x94 2560\xC3\x97"
                     "1440 \xC2\xB7 165 Hz";
    primary.width = 2560;
    primary.height = 1440;
    primary.refreshMilliHz = 164998;
    primary.gpuId = 0;
    primary.hdrActive = true;
    primary.primary = true;

    DisplayInfo second;
    second.id = 2;
    second.label = "Display 3";
    second.detail = "LINDY \xE2\x80\x94 1920\xC3\x97"
                    "1080 \xC2\xB7 60 Hz";
    second.width = 1920;
    second.height = 1080;
    second.refreshMilliHz = 60000;
    second.gpuId = 1;

    caps.displays = {primary, second};
    return caps;
}

} // namespace

void run_native_capabilities_json_tests()
{
    SECTION("NativeCapabilitiesJson — an available machine survives the round trip");
    {
        const Capabilities in = sample();
        Capabilities out;
        CHECK(NativeCapabilitiesJson::fromJson(NativeCapabilitiesJson::toJson(in), out));

        CHECK(out.available);
        CHECK(out.reason == Unavailability::None);
        CHECK(out.capture == CaptureApi::DxgiDuplication);

        CHECK(out.gpus.size() == 2);
        CHECK(out.gpus[0].id == 0);
        CHECK(out.gpus[0].name == "NVIDIA GeForce RTX 5060 Ti");
        CHECK(out.gpus[0].vendorId == 0x10DE);
        CHECK(out.gpus[0].deviceId == 0x2D04);
        CHECK(out.gpus[0].nativeHandle == 0xFFFFFFFF0000ABCDull);
        CHECK(out.gpus[0].encoders.size() == 1 && out.gpus[0].encoders[0] == EncoderApi::Nvenc);
        CHECK(out.gpus[0].codecs.size() == 3 && out.gpus[0].codecs[0] == Codec::Av1);
        CHECK(out.gpus[0].supports444(Codec::Hevc));
        CHECK(out.gpus[0].supports444(Codec::H264));
        CHECK(!out.gpus[0].supports444(Codec::Av1));
        CHECK(out.gpus[0].supports10Bit);
        // A GPU with no encoder comes back as exactly that — not dropped, not
        // given one.
        CHECK(out.gpus[1].encoders.empty());
        CHECK(out.gpus[1].codecs.empty());
        CHECK(!out.gpus[1].supports10Bit);

        CHECK(out.displays.size() == 2);
        CHECK(out.displays[0].id == 0);
        CHECK(out.displays[0].label == "Display 1");
        CHECK(out.displays[0].detail == in.displays[0].detail);
        CHECK(out.displays[0].width == 2560 && out.displays[0].height == 1440);
        CHECK(out.displays[0].refreshMilliHz == 164998);
        CHECK(out.displays[0].hdrActive);
        CHECK(out.displays[0].primary);
        // Display ids are not indices: a gap (id 2 in second place) is kept.
        CHECK(out.displays[1].id == 2);
        CHECK(out.displays[1].gpuId == 1);
        CHECK(!out.displays[1].hdrActive);
        CHECK(!out.displays[1].primary);

        // gpuFor() works on the copy as it did on the original.
        CHECK(out.gpuFor(out.displays[0]) == &out.gpus[0]);
        CHECK(out.gpuFor(out.displays[1]) == &out.gpus[1]);

        // And the serialization is stable: the service compares two snapshots
        // by their JSON to decide whether the host card has to move.
        CHECK(NativeCapabilitiesJson::toJson(in) == NativeCapabilitiesJson::toJson(out));
    }

    SECTION("NativeCapabilitiesJson — an unavailable machine keeps its reason");
    {
        Capabilities in;
        in.available = false;
        in.reason = Unavailability::NoEncoder;
        in.diagnostic = "no hardware encoder on any GPU";
        Capabilities out;
        CHECK(NativeCapabilitiesJson::fromJson(NativeCapabilitiesJson::toJson(in), out));
        CHECK(!out.available);
        CHECK(out.reason == Unavailability::NoEncoder);
        CHECK(out.diagnostic == "no hardware encoder on any GPU");
        CHECK(out.gpus.empty() && out.displays.empty());
    }

    SECTION("NativeCapabilitiesJson — a reply from another build is refused whole");
    {
        QJsonObject foreign = NativeCapabilitiesJson::toJson(sample());
        foreign["schema"] = NativeCapabilitiesJson::kSchema + 1;
        Capabilities out;
        CHECK(!NativeCapabilitiesJson::fromJson(foreign, out));
        CHECK(!out.available);
        CHECK(out.reason == Unavailability::ProbeFailed);
        CHECK(out.displays.empty());
        CHECK(!out.diagnostic.empty());

        // Not a probe result at all.
        CHECK(!NativeCapabilitiesJson::fromJson(QJsonObject{}, out));
        CHECK(!out.available);
    }

    SECTION("NativeCapabilitiesJson — one compact line, as the pipe carries it");
    {
        const QByteArray line =
            QJsonDocument(NativeCapabilitiesJson::toJson(sample())).toJson(QJsonDocument::Compact);
        CHECK(!line.contains('\n'));
        Capabilities out;
        CHECK(NativeCapabilitiesJson::fromJson(QJsonDocument::fromJson(line).object(), out));
        CHECK(out.available && out.displays.size() == 2);
    }
}
