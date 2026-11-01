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

#include <QJsonArray>
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
    // Above 2^53 too, as every driver version is.
    nvidia.driverVersion = driverVersionOf(32, 0, 15, 7652);
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
    primary.kind = DisplayKind::BuiltIn;
    primary.model = "M27Q";
    primary.key = "\\\\?\\DISPLAY#GBT270D#5&1f2a&0&UID4352";

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
    caps.hasBattery = true;
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
        CHECK(out.gpus[0].driverVersion == driverVersionOf(32, 0, 15, 7652));
        CHECK(driverVersionText(out.gpus[0].driverVersion) == "32.0.15.7652");
        // A GPU whose driver nobody gave keeps none, and an older probe's
        // answer, which never carried one, reads the same.
        CHECK(out.gpus[1].driverVersion == 0);
        {
            QJsonObject older = NativeCapabilitiesJson::toJson(in);
            QJsonArray gpus = older["gpus"].toArray();
            QJsonObject first = gpus[0].toObject();
            first.remove("driverVersion");
            gpus[0] = first;
            older["gpus"] = gpus;
            Capabilities fromOlder;
            CHECK(NativeCapabilitiesJson::fromJson(older, fromOlder));
            CHECK(fromOlder.gpus[0].driverVersion == 0);
        }
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
        // What the card draws: the kind of screen, its name, its stable key,
        // and whether the machine has a battery.
        CHECK(out.displays[0].kind == DisplayKind::BuiltIn);
        CHECK(out.displays[0].model == "M27Q");
        CHECK(out.displays[0].key == in.displays[0].key);
        CHECK(out.displays[1].kind == DisplayKind::Unknown);
        CHECK(out.displays[1].model.empty() && out.displays[1].key.empty());
        CHECK(out.hasBattery);

        // gpuFor() works on the copy as it did on the original.
        CHECK(out.gpuFor(out.displays[0]) == &out.gpus[0]);
        CHECK(out.gpuFor(out.displays[1]) == &out.gpus[1]);

        // And the serialization is stable: the service compares two snapshots
        // by their JSON to decide whether the host card has to move.
        CHECK(NativeCapabilitiesJson::toJson(in) == NativeCapabilitiesJson::toJson(out));
    }

    SECTION("NativeCapabilitiesJson — the GPU a display card names, numbered among namesakes");
    {
        Capabilities caps = sample();
        // Two GPUs of different names: each display names its own, no number.
        QJsonObject gpu = NativeCapabilitiesJson::displayGpu(caps, 0);
        CHECK(gpu.value("gpu").toString() == QStringLiteral("NVIDIA GeForce RTX 5060 Ti"));
        CHECK(!gpu.contains("gpuNumber"));
        CHECK(NativeCapabilitiesJson::displayGpu(caps, 1).value("gpu").toString() ==
              QStringLiteral("Microsoft Basic Render Driver"));
        // A GPU nobody has: nothing said.
        CHECK(NativeCapabilitiesJson::displayGpu(caps, 7).isEmpty());
        CHECK(NativeCapabilitiesJson::displayGpu(caps, -1).isEmpty());

        // DualRTX: two RTX of one name around another GPU. Counted in the OS's
        // order among the namesakes only.
        GpuInfo twin = caps.gpus[0];
        twin.id = 2;
        caps.gpus.push_back(twin);
        gpu = NativeCapabilitiesJson::displayGpu(caps, 0);
        CHECK(gpu.value("gpuNumber").toInt() == 1);
        gpu = NativeCapabilitiesJson::displayGpu(caps, 2);
        CHECK(gpu.value("gpu").toString() == QStringLiteral("NVIDIA GeForce RTX 5060 Ti"));
        CHECK(gpu.value("gpuNumber").toInt() == 2);
        CHECK(!NativeCapabilitiesJson::displayGpu(caps, 1).contains("gpuNumber"));

        // One GPU: the line would say nothing, so there is none.
        caps.gpus.resize(1);
        CHECK(NativeCapabilitiesJson::displayGpu(caps, 0).isEmpty());
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

    SECTION("NativeCapabilitiesJson — a missing input permission travels, an absent key does not "
            "invent one");
    {
        // macOS Accessibility: the grant whose refusal is silent. A machine
        // that streams a picture nobody can drive is still `available`, so the
        // flag is the ONLY thing carrying that state across the pipe.
        Capabilities in = sample();
        in.inputPermission = false;
        Capabilities out;
        CHECK(NativeCapabilitiesJson::fromJson(NativeCapabilitiesJson::toJson(in), out));
        CHECK(out.available);
        CHECK(!out.inputPermission);

        // An older probe binary answering a newer server says nothing about it.
        // "Nothing to report" is the right reading: every platform but macOS
        // asks nobody, and a warning invented here would show on every PC.
        QJsonObject older = NativeCapabilitiesJson::toJson(in);
        older.remove("inputPermission");
        CHECK(NativeCapabilitiesJson::fromJson(older, out));
        CHECK(out.inputPermission);
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
