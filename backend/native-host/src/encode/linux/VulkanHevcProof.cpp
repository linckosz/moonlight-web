/*
 * MoonlightWeb — native capture & encoding engine.
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

#include "VulkanHevcProof.h"

#include "../../core/Log.h"
#include "../../platform/linux/vulkan/VulkanDevice.h"
#include "Dav1dDecoder.h"
#include "VulkanAv1Encoder.h"
#include "VulkanHevcDecoder.h"

#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace mw::native::encode {
namespace {

/// This engine's encoder, as far as a verdict goes: raised whenever what
/// VulkanHevcEncoder hands the driver changes (its parameter sets, its use of
/// references), or how the proof reads it back, so that a verdict reached by
/// another revision counts for nothing. 2: intra refresh (C13.9), which the
/// proof now covers. 3: the proof's decoder lists every picture kept; until
/// then it read a right stream wrong from 73 fps on, and the failure was kept
/// for good (09/10/2026).
constexpr int kEncoderRevision = 3;

/// The sequence: an IDR, P pictures, frames 4 and 5 lost and healed from an
/// older picture kept — 3 up to 72 fps, the IDR from 73 on, where the slots'
/// stride passes 3 by (ReferenceSlots) — a keyframe asked for at 10.
constexpr int kPictures = 12;
constexpr uint32_t kLostFrom = 4;
constexpr uint32_t kLostTo = 5;
constexpr uint32_t kKeyframeAt = 10;

/// Where the driver has intra refresh, sweeps of this many pictures back to
/// back: pictures 4 to 7 would be the first, the repair at 6 starts it over
/// from an older picture (a wholly dirty reference), and the keyframe at 10
/// ends the next — every way a sweep is told to the driver, in the same dozen
/// pictures.
constexpr int kProofSweep = 4;

/// A right stream sits far above these; the wrong ones of §8o.3 decoded at
/// 5 dB, and a single wrong CTB row falls under the band's.
constexpr double kPictureFloorDb = 22.0;
constexpr double kBandFloorDb = 18.0;

/// The proof's pictures, NV12: the lab's (tools/vk-lab, encode), a ramp that
/// moves, fine stripes, blocks that come and go — the ones that caught the
/// 780M's fault, where a still picture did not.
std::vector<uint8_t> proofPicture(int width, int height, int n)
{
    const size_t w = static_cast<size_t>(width), h = static_cast<size_t>(height);
    std::vector<uint8_t> out(w * h * 3 / 2);
    uint8_t* y = out.data();
    uint8_t* uv = y + w * h;
    const uint32_t k = static_cast<uint32_t>(n);
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t col = 0; col < w; ++col) {
            int v = 40 + static_cast<int>((col + row / 2 + k * 6) % 160);
            if (row % 90 < 30) v = ((col / 2 + k) % 2) ? 200 : 40;
            if ((col / 16 + row / 16 + k) % 7 == 0) v = 235 - v / 4;
            y[row * w + col] = static_cast<uint8_t>(std::clamp(v, 16, 235));
        }
    }
    for (uint32_t row = 0; row < h / 2; ++row) {
        for (uint32_t col = 0; col < w / 2; ++col) {
            uv[row * w + 2 * col] = static_cast<uint8_t>(96 + (col + k * 3) % 64);
            uv[row * w + 2 * col + 1] = static_cast<uint8_t>(104 + (row + k * 5) % 48);
        }
    }
    return out;
}

double psnr(double squaredError, double samples)
{
    if (samples <= 0) return 0.0;
    const double mse = squaredError / samples;
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::string firstLine(const std::string& path)
{
    std::ifstream in(path);
    std::string line;
    if (in) std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

/// The AV1 encoder's revision, as kEncoderRevision is HEVC's: raised whenever
/// what VulkanAv1Encoder hands the driver changes. 1: C13.12. 2: the frame
/// padded past the picture, as for a viewer that crops.
constexpr int kAv1EncoderRevision = 2;

#ifndef MW_APP_VERSION
#define MW_APP_VERSION ""
#endif

/// What could change a verdict, as one line. AV1's keys start with "av1|" and
/// carry its own revision and the decoder. Both end with the app's version:
/// a revision is raised only for a change known to matter, and a verdict, a
/// failure above all, must not outlive the build that reached it — every
/// update asks again (Bruno, 09/10/2026).
std::string cacheKey(const vulkan::DeviceIdentity& id, const std::string& renderNode, int width,
                     int height, const VulkanHevcEncoder::Witness& witness,
                     const std::string& av1Decoder = std::string())
{
    std::string uuid;
    char hex[4];
    for (uint8_t b : id.uuid) {
        std::snprintf(hex, sizeof(hex), "%02x", b);
        uuid += hex;
    }
    utsname un = {};
    const std::string kernel = ::uname(&un) == 0 ? un.release : "?";
    // amdgpu shows its VCN firmware to everyone; other drivers have none here.
    const std::string node = renderNode.substr(renderNode.find_last_of('/') + 1);
    const std::string vcn =
        firstLine("/sys/class/drm/" + node + "/device/fw_version/vcn_fw_version");
    std::ostringstream key;
    if (!av1Decoder.empty()) key << "av1|";
    key << uuid << '|' << std::hex << id.vendorId << std::dec << '|' << id.driverVersion << '|'
        << id.name << '|' << kernel << '|' << (vcn.empty() ? "-" : vcn) << '|' << width << 'x'
        << height;
    if (av1Decoder.empty())
        key << "|r" << kEncoderRevision << "|d" << witness.transformDepth;
    else
        key << "|r" << kAv1EncoderRevision << "|q" << witness.constantQp << '|' << av1Decoder;
    const std::string version = MW_APP_VERSION;
    key << "|v" << (version.empty() ? "-" : version);
    std::string text = key.str();
    std::replace(text.begin(), text.end(), '\t', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

std::string cachePath()
{
    // A process the package's launcher starts (the stream worker)
    // runs with its home and cache folders fixed, and finds the user's home
    // under MW_USER_HOME instead (moonlightweb-launch-env.h).
    const char* user = std::getenv("MW_USER_HOME");
    if (user && *user == '/')
        return std::string(user) + "/.cache/MoonlightWeb/vulkan-video-proofs.txt";
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg == '/') return std::string(xdg) + "/MoonlightWeb/vulkan-video-proofs.txt";
    const char* home = std::getenv("HOME");
    if (home && *home == '/')
        return std::string(home) + "/.cache/MoonlightWeb/vulkan-video-proofs.txt";
    return {};
}

std::mutex& cacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// One line per key: key, pass|fail, worst picture dB, worst band dB,
/// pictures, summary — tab-separated.
bool readCache(const std::string& key, VulkanHevcProof& out)
{
    const std::string path = cachePath();
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lock(cacheMutex());
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> field;
        std::stringstream split(line);
        std::string part;
        while (std::getline(split, part, '\t'))
            field.push_back(part);
        if (field.size() < 6 || field[0] != key) continue;
        out = VulkanHevcProof{};
        out.ran = true;
        out.passed = field[1] == "pass";
        out.worstPicturePsnr = std::atof(field[2].c_str());
        out.worstBandPsnr = std::atof(field[3].c_str());
        out.pictures = std::atoi(field[4].c_str());
        out.summary = field[5];
        out.cached = true;
        return true;
    }
    return false;
}

void writeCache(const std::string& key, const VulkanHevcProof& proof)
{
    const std::string path = cachePath();
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(cacheMutex());
    // The directories, one level at a time; they may all exist already.
    for (size_t slash = path.find('/', 1); slash != std::string::npos;
         slash = path.find('/', slash + 1))
        ::mkdir(path.substr(0, slash).c_str(), 0700);
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty() && line.compare(0, key.size() + 1, key + '\t') != 0)
                lines.push_back(line);
    }
    char numbers[64];
    std::snprintf(numbers, sizeof(numbers), "%.2f\t%.2f\t%d", proof.worstPicturePsnr,
                  proof.worstBandPsnr, proof.pictures);
    std::string summary = proof.summary;
    std::replace(summary.begin(), summary.end(), '\t', ' ');
    std::replace(summary.begin(), summary.end(), '\n', ' ');
    lines.push_back(key + '\t' + (proof.passed ? "pass" : "fail") + '\t' + numbers + '\t' +
                    summary);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const std::string& line : lines)
            out << line << '\n';
        if (!out) return;
    }
    ::rename(temporary.c_str(), path.c_str());
}

} // namespace

PicturePsnr comparePictures(const uint8_t* reference, const uint8_t* decoded, int width, int height,
                            int bandRows)
{
    PicturePsnr q;
    const size_t w = static_cast<size_t>(std::max(width, 0));
    const size_t h = static_cast<size_t>(std::max(height, 0));
    const size_t band = static_cast<size_t>(std::max(bandRows, 1));
    double total = 0.0, bandError = 0.0;
    q.worstBand = 99.0;
    for (size_t row = 0; row < h; ++row) {
        const uint8_t* a = reference + row * w;
        const uint8_t* b = decoded + row * w;
        double sum = 0.0;
        for (size_t col = 0; col < w; ++col) {
            const double e = static_cast<double>(a[col]) - static_cast<double>(b[col]);
            sum += e * e;
        }
        total += sum;
        bandError += sum;
        if ((row + 1) % band == 0 || row + 1 == h) {
            const size_t rows = (row % band) + 1;
            q.worstBand = std::min(q.worstBand, psnr(bandError, static_cast<double>(rows * w)));
            bandError = 0.0;
        }
    }
    q.luma = psnr(total, static_cast<double>(w * h));
    double chroma = 0.0;
    const size_t chromaBytes = w * (h / 2);
    for (size_t i = 0; i < chromaBytes; ++i) {
        const double e = static_cast<double>(reference[w * h + i]) - decoded[w * h + i];
        chroma += e * e;
    }
    q.chroma = psnr(chroma, static_cast<double>(chromaBytes));
    return q;
}

VulkanHevcEncoder::Witness witnessFromEnvironment()
{
    VulkanHevcEncoder::Witness witness;
    if (const char* depth = std::getenv("MW_VK_ENCODE_DEPTH"); depth && *depth)
        witness.transformDepth = std::atoi(depth);
    return witness;
}

VulkanHevcProof proveVulkanHevc(const std::string& renderNode, int width, int height, int fps,
                                const EncoderTuning& tuning,
                                const VulkanHevcEncoder::Witness& witness)
{
    VulkanHevcProof proof;
    const auto started = std::chrono::steady_clock::now();
    auto took = [&] {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count());
    };

    // Its own device, with a decoder beside the encoder: the verdict is the
    // GPU's and the driver's, not the session's device's.
    vulkan::DeviceOptions options;
    options.wantHigh = false;
    options.encodeHevc = true;
    options.decodeHevc = true;
    std::string error;
    std::shared_ptr<vulkan::VulkanDevice> device =
        vulkan::VulkanDevice::open(renderNode, options, error);
    if (!device) {
        proof.summary = error;
        proof.tookMs = took();
        return proof;
    }
    // The product's rate control at a budget per picture that leaves the
    // comparison to the coding, not to the rate: 50 Mbit/s at 1080p60.
    const double scale =
        static_cast<double>(width) * height / (1920.0 * 1080.0) * (fps > 0 ? fps : 60) / 60.0;
    const int kbps = std::max(10000, static_cast<int>(50000.0 * scale));
    VulkanHevcEncoder::Witness sweeping = witness;
    sweeping.sweepPictures = kProofSweep;
    VulkanHevcEncoder encoder;
    if (!encoder.init(device, Codec::Hevc, width, height, fps, kbps, /*intraRefresh=*/true, tuning,
                      error, sweeping)) {
        proof.summary = error;
        proof.tookMs = took();
        return proof;
    }
    VulkanHevcDecoder decoder;
    if (!decoder.init(device, encoder.parameterSets(), error)) {
        proof.summary = "the pixel proof cannot read the stream back: " + error;
        proof.tookMs = took();
        return proof;
    }
    if (decoder.width() != width || decoder.height() != height) {
        proof.summary = "the stream crops to " + std::to_string(decoder.width()) + "x" +
                        std::to_string(decoder.height()) + ", not " + std::to_string(width) + "x" +
                        std::to_string(height);
        proof.tookMs = took();
        return proof;
    }

    const int bandRows = 1 << encoder.sps().log2CodingTreeBlock;
    double worstPicture = 99.0, worstBand = 99.0, worstChroma = 99.0;
    int decoded = 0, worstAt = -1;
    std::vector<uint8_t> out;
    for (int n = 0; n < kPictures; ++n) {
        const uint32_t frame = static_cast<uint32_t>(n);
        const std::vector<uint8_t> picture = proofPicture(width, height, n);
        if (!encoder.upload(picture.data(), error)) {
            proof.summary = "picture " + std::to_string(n) + ": " + error;
            proof.tookMs = took();
            return proof;
        }
        // The receiver reports frames 4 and 5 lost before 6 is encoded.
        if (frame == kLostTo + 1 && !encoder.invalidateReference(kLostFrom, error)) {
            proof.summary = "the loss of frame " + std::to_string(kLostFrom) + ": " + error;
            proof.tookMs = took();
            return proof;
        }
        EncoderOutput encoded;
        if (!encoder.encode(frame == kKeyframeAt, frame, encoded, error)) {
            proof.summary = "picture " + std::to_string(n) + ": " + error;
            proof.tookMs = took();
            return proof;
        }
        const std::vector<uint8_t> unit(encoded.data, encoded.data + encoded.size);
        encoder.releaseOutput();
        if (frame >= kLostFrom && frame <= kLostTo) continue; // never arrived
        if (!decoder.decode(unit.data(), unit.size(), out, error)) {
            proof.summary = "picture " + std::to_string(n) + " does not decode: " + error;
            proof.tookMs = took();
            return proof;
        }
        const PicturePsnr q = comparePictures(picture.data(), out.data(), width, height, bandRows);
        if (q.luma < worstPicture || q.worstBand < worstBand) worstAt = n;
        worstPicture = std::min(worstPicture, q.luma);
        worstBand = std::min(worstBand, q.worstBand);
        worstChroma = std::min(worstChroma, q.chroma);
        ++decoded;
    }

    proof.ran = true;
    proof.pictures = decoded;
    proof.worstPicturePsnr = worstPicture;
    proof.worstBandPsnr = worstBand;
    proof.passed = worstPicture >= kPictureFloorDb && worstBand >= kBandFloorDb;
    char numbers[200];
    std::snprintf(numbers, sizeof(numbers),
                  "%d pictures decoded back, worst %.1f dB over a picture and %.1f dB over a CTB "
                  "row (picture %d), chroma %.1f dB, %s",
                  decoded, worstPicture, worstBand, worstAt, worstChroma,
                  encoder.intraRefreshEnabled() ? "with intra-refresh sweeps"
                                                : "no intra refresh on this driver");
    proof.summary =
        proof.passed ? std::string(numbers)
                     : "the stream does not decode to what was encoded: " + std::string(numbers) +
                           " (transform depth " +
                           std::to_string(encoder.sps().maxTransformHierarchyDepthInter) + ")";
    proof.tookMs = took();
    return proof;
}

VulkanHevcProof vulkanHevcVerdict(const std::string& renderNode, int width, int height, int fps,
                                  const EncoderTuning& tuning)
{
    const VulkanHevcEncoder::Witness witness = witnessFromEnvironment();
    if (witness.transformDepth >= 0)
        log::info("[native] MW_VK_ENCODE_DEPTH in effect: the Vulkan encoder codes transform "
                  "depth " +
                  std::to_string(witness.transformDepth));
    // Nothing to prove where the driver shows no encoder: said at once,
    // without opening a device.
    vulkan::DeviceIdentity id;
    std::string error;
    if (!vulkan::VulkanDevice::identify(renderNode, id, error)) {
        VulkanHevcProof proof;
        proof.summary = error;
        return proof;
    }
    if (!id.encodesHevc) {
        VulkanHevcProof proof;
        proof.summary = id.name + " shows no Vulkan Video HEVC encoder";
        return proof;
    }
    const char* cacheSetting = std::getenv("MW_VK_PROOF_CACHE");
    const bool useCache = !(cacheSetting && std::string(cacheSetting) == "0");
    const std::string key = cacheKey(id, renderNode, width, height, witness);
    VulkanHevcProof proof;
    if (useCache && readCache(key, proof)) {
        log::info("[native] Vulkan Video pixel proof on " + id.name + ", " + std::to_string(width) +
                  "x" + std::to_string(height) + ": " + (proof.passed ? "passed" : "failed") +
                  " (kept from an earlier proof) — " + proof.summary);
        return proof;
    }
    proof = proveVulkanHevc(renderNode, width, height, fps, tuning, witness);
    log::info("[native] Vulkan Video pixel proof on " + id.name + ", " + std::to_string(width) +
              "x" + std::to_string(height) + ": " +
              (proof.ran ? (proof.passed ? "passed" : "FAILED") : "could not run") + " in " +
              std::to_string(proof.tookMs) + " ms — " + proof.summary);
    if (useCache && proof.ran) writeCache(key, proof);
    return proof;
}

VulkanHevcProof proveVulkanAv1(const std::string& renderNode, int width, int height, int fps,
                               const EncoderTuning& tuning, const VulkanEncodeWitness& witness)
{
    VulkanHevcProof proof;
    const auto started = std::chrono::steady_clock::now();
    auto took = [&] {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count());
    };
    auto refused = [&](const std::string& why) {
        proof.summary = why;
        proof.tookMs = took();
        return proof;
    };

    std::string error;
    Dav1dDecoder decoder;
    if (!decoder.open(error)) return refused(error);
    // Its own device: the verdict is the GPU's and the driver's.
    vulkan::DeviceOptions options;
    options.wantHigh = false;
    options.encodeAv1 = true;
    std::shared_ptr<vulkan::VulkanDevice> device =
        vulkan::VulkanDevice::open(renderNode, options, error);
    if (!device) return refused(error);
    const double scale =
        static_cast<double>(width) * height / (1920.0 * 1080.0) * (fps > 0 ? fps : 60) / 60.0;
    const int kbps = std::max(10000, static_cast<int>(50000.0 * scale));
    VulkanEncodeWitness sweeping = witness;
    sweeping.sweepPictures = kProofSweep;
    // The frame padded past the picture, as a viewer that crops is sent it:
    // the larger frame, and the render size the stream depends on there. A
    // stream on the driver's grid (alignedToGrid) codes a smaller frame with
    // the same encoder.
    VulkanAv1Encoder encoder;
    encoder.setClientCrops(true);
    if (!encoder.init(device, Codec::Av1, width, height, fps, kbps, /*intraRefresh=*/true, tuning,
                      error, sweeping))
        return refused(error);
    width = encoder.input().width;
    height = encoder.input().height;

    const int bandRows = encoder.sequence().sb128 ? 128 : 64;
    double worstPicture = 99.0, worstBand = 99.0, worstChroma = 99.0;
    int decoded = 0, worstAt = -1;
    std::vector<uint8_t> out;
    for (int n = 0; n < kPictures; ++n) {
        const uint32_t frame = static_cast<uint32_t>(n);
        const std::vector<uint8_t> picture = proofPicture(width, height, n);
        if (!encoder.upload(picture.data(), error))
            return refused("picture " + std::to_string(n) + ": " + error);
        if (frame == kLostTo + 1 && !encoder.invalidateReference(kLostFrom, error))
            return refused("the loss of frame " + std::to_string(kLostFrom) + ": " + error);
        EncoderOutput encoded;
        if (!encoder.encode(frame == kKeyframeAt, frame, encoded, error))
            return refused("picture " + std::to_string(n) + ": " + error);
        const std::vector<uint8_t> unit(encoded.data, encoded.data + encoded.size);
        encoder.releaseOutput();
        if (frame >= kLostFrom && frame <= kLostTo) continue; // never arrived
        // The frame at the encoder's alignment; its top left is the picture
        // (the render size, which the encoder's guard reads back).
        int frameWidth = 0, frameHeight = 0;
        if (!decoder.decode(unit.data(), unit.size(), width, height, out, frameWidth, frameHeight,
                            error))
            return refused("picture " + std::to_string(n) + " does not decode: " + error);
        const PicturePsnr q = comparePictures(picture.data(), out.data(), width, height, bandRows);
        if (q.luma < worstPicture || q.worstBand < worstBand) worstAt = n;
        worstPicture = std::min(worstPicture, q.luma);
        worstBand = std::min(worstBand, q.worstBand);
        worstChroma = std::min(worstChroma, q.chroma);
        ++decoded;
    }

    proof.ran = true;
    proof.pictures = decoded;
    proof.worstPicturePsnr = worstPicture;
    proof.worstBandPsnr = worstBand;
    proof.passed = worstPicture >= kPictureFloorDb && worstBand >= kBandFloorDb;
    char numbers[240];
    std::snprintf(numbers, sizeof(numbers),
                  "%d pictures decoded back by %s, worst %.1f dB over a picture and %.1f dB over a "
                  "superblock row (picture %d), chroma %.1f dB, %s",
                  decoded, decoder.version().c_str(), worstPicture, worstBand, worstAt, worstChroma,
                  encoder.intraRefreshEnabled() ? "with intra-refresh sweeps"
                                                : "no intra refresh on this driver");
    proof.summary = proof.passed ? std::string(numbers)
                                 : "the AV1 stream does not decode to what was encoded: " +
                                       std::string(numbers);
    proof.tookMs = took();
    return proof;
}

VulkanHevcProof vulkanAv1Verdict(const std::string& renderNode, int width, int height, int fps,
                                 const EncoderTuning& tuning)
{
    const VulkanEncodeWitness witness;
    vulkan::DeviceIdentity id;
    std::string error;
    if (!vulkan::VulkanDevice::identify(renderNode, id, error)) {
        VulkanHevcProof proof;
        proof.summary = error;
        return proof;
    }
    if (!id.encodesAv1) {
        VulkanHevcProof proof;
        proof.summary = id.name + " shows no Vulkan Video AV1 encoder";
        return proof;
    }
    // The decoder is part of what the verdict holds for: opened first, and a
    // machine without one is told so before any device is.
    std::string decoderName;
    {
        Dav1dDecoder probe;
        if (!probe.open(error)) {
            VulkanHevcProof proof;
            proof.summary = error;
            return proof;
        }
        decoderName = probe.version();
    }
    const char* cacheSetting = std::getenv("MW_VK_PROOF_CACHE");
    const bool useCache = !(cacheSetting && std::string(cacheSetting) == "0");
    const std::string key = cacheKey(id, renderNode, width, height, witness, decoderName);
    VulkanHevcProof proof;
    if (useCache && readCache(key, proof)) {
        log::info("[native] Vulkan Video AV1 pixel proof on " + id.name + ", " +
                  std::to_string(width) + "x" + std::to_string(height) + ": " +
                  (proof.passed ? "passed" : "failed") + " (kept from an earlier proof) — " +
                  proof.summary);
        return proof;
    }
    proof = proveVulkanAv1(renderNode, width, height, fps, tuning, witness);
    log::info("[native] Vulkan Video AV1 pixel proof on " + id.name + ", " + std::to_string(width) +
              "x" + std::to_string(height) + ": " +
              (proof.ran ? (proof.passed ? "passed" : "FAILED") : "could not run") + " in " +
              std::to_string(proof.tookMs) + " ms — " + proof.summary);
    if (useCache && proof.ran) writeCache(key, proof);
    return proof;
}

} // namespace mw::native::encode
