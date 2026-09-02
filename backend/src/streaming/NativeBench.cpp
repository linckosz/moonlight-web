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

#include "NativeBench.h"

#include "mw/native/NativeHost.h"
#include "mw/native/StageStats.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchSpec
{
    int display = -1;
    int seconds = 10;
    QString out;
    mw::native::Codec codec = mw::native::Codec::Hevc;
    int fps = 0;
    int bitrateKbps = 20000;
    int width = 0;
    int height = 0;
    bool yuv444 = false;
    bool intraRefresh = false;
};

/// What one frame cost, copied out of the callback. The bytes themselves are
/// not kept: this is a sink, and the point is that nothing downstream of the
/// encoder is being measured here.
struct BenchRow
{
    uint32_t frameNumber = 0;
    bool keyframe = false;
    /// False for a re-send of a still picture (no capture happened).
    bool captured = false;
    size_t bytes = 0;
    int avgQp = -1;
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
    int64_t submittedUs = 0;
    int64_t convertedUs = 0;
    int64_t dueUs = 0;
    int64_t encodedUs = 0;
};

const char* const kUsage =
    "usage: --native-bench <key=value,...>\n"
    "  display=<id>     the display to capture (omit to list them)\n"
    "  seconds=<n>      how long to run (default 10)\n"
    "  codec=hevc|h264|av1   (default hevc)\n"
    "  fps=<n>          stream frame rate, 0 = the display's own (default 0)\n"
    "  bitrate=<kbps>   (default 20000)\n"
    "  width=<px>,height=<px>   output size, 0 = the display's (default 0)\n"
    "  yuv444=0|1       (default 0)\n"
    "  intra=0|1        intra-refresh instead of keyframes (default 0)\n"
    "  out=<path.csv>   one row per frame (default native-bench-<time>.csv here)\n";

bool parseSpec(const QString& text, BenchSpec& spec, QString& error)
{
    const QStringList items = text.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    for (const QString& raw : items) {
        const QString item = raw.trimmed();
        const int eq = item.indexOf('=');
        if (eq <= 0) {
            error = "not a key=value pair: " + item;
            return false;
        }
        const QString key = item.left(eq).trimmed().toLower();
        const QString value = item.mid(eq + 1).trimmed();
        bool ok = true;
        if (key == "display")
            spec.display = value.toInt(&ok);
        else if (key == "seconds")
            spec.seconds = value.toInt(&ok);
        else if (key == "fps")
            spec.fps = value.toInt(&ok);
        else if (key == "bitrate")
            spec.bitrateKbps = value.toInt(&ok);
        else if (key == "width")
            spec.width = value.toInt(&ok);
        else if (key == "height")
            spec.height = value.toInt(&ok);
        else if (key == "yuv444")
            spec.yuv444 = value.toInt(&ok) != 0;
        else if (key == "intra")
            spec.intraRefresh = value.toInt(&ok) != 0;
        else if (key == "out")
            spec.out = value;
        else if (key == "codec") {
            const QString c = value.toLower();
            if (c == "hevc" || c == "h265")
                spec.codec = mw::native::Codec::Hevc;
            else if (c == "h264" || c == "avc")
                spec.codec = mw::native::Codec::H264;
            else if (c == "av1")
                spec.codec = mw::native::Codec::Av1;
            else
                ok = false;
        } else {
            error = "unknown key: " + key;
            return false;
        }
        if (!ok) {
            error = "bad value for " + key + ": " + value;
            return false;
        }
    }
    if (spec.seconds <= 0 || spec.seconds > 3600) {
        error = "seconds must be between 1 and 3600";
        return false;
    }
    return true;
}

QString describeDisplay(const mw::native::DisplayInfo& d)
{
    return QString("  display=%1  %2  %3x%4 @ %5 Hz  gpu %6%7")
        .arg(d.id)
        .arg(QString::fromStdString(d.label), -10)
        .arg(d.width)
        .arg(d.height)
        .arg(d.refreshMilliHz / 1000.0, 0, 'f', 2)
        .arg(d.gpuId)
        .arg(d.detail.empty() ? QString() : "  (" + QString::fromStdString(d.detail) + ")");
}

/// mean / p95 / p99 of an integer quantity, with a unit divisor.
QString tail(const mw::native::LatencyHistogram& h, double divisor, int decimals)
{
    if (h.count() == 0) return "-";
    return QString("%1 / %2 / %3")
        .arg(h.meanUs() / divisor, 0, 'f', decimals)
        .arg(h.percentileUs(0.95) / divisor, 0, 'f', decimals)
        .arg(h.percentileUs(0.99) / divisor, 0, 'f', decimals);
}

} // namespace

int runNativeBenchCommand(const QString& specText)
{
    QTextStream out(stdout);
    QTextStream err(stderr);

    BenchSpec spec;
    QString parseError;
    if (!parseSpec(specText, spec, parseError)) {
        err << "native-bench: " << parseError << "\n\n" << kUsage;
        err.flush();
        return 2;
    }

    // The engine explains itself in its log — which GPU, which encoder, why a
    // fallback — and a bench that hid that would leave a bad number with
    // nothing to go on.
    mw::native::NativeHost::setLogSink([](int level, const std::string& message) {
        static const char* const kNames[] = {"debug", "info", "warn", "error"};
        const char* name = (level >= 0 && level <= 3) ? kNames[level] : "?";
        std::fprintf(stderr, "[%s] %s\n", name, message.c_str());
    });

    const mw::native::Capabilities caps = mw::native::NativeHost::probe();
    if (!caps.available) {
        err << "native-bench: the native engine is not available here: "
            << mw::native::toString(caps.reason)
            << (caps.diagnostic.empty() ? QString()
                                        : " (" + QString::fromStdString(caps.diagnostic) + ")")
            << "\n";
        err.flush();
        return 1;
    }

    const mw::native::DisplayInfo* display = nullptr;
    for (const mw::native::DisplayInfo& d : caps.displays)
        if (d.id == spec.display) display = &d;
    if (!display) {
        if (spec.display >= 0) err << "native-bench: no display with id " << spec.display << "\n";
        out << "Displays:\n";
        for (const mw::native::DisplayInfo& d : caps.displays)
            out << describeDisplay(d) << "\n";
        out << "\nRun again with display=<id>.\n";
        out.flush();
        err.flush();
        return spec.display >= 0 ? 1 : 2;
    }

    mw::native::SessionConfig config;
    config.displayId = spec.display;
    config.width = spec.width;
    config.height = spec.height;
    config.fps = spec.fps;
    config.bitrateKbps = spec.bitrateKbps;
    config.clientCodecs = {spec.codec};
    config.yuv444 = spec.yuv444;
    config.intraRefresh = spec.intraRefresh;

    std::mutex rowsMutex;
    std::vector<BenchRow> rows;
    rows.reserve(static_cast<size_t>(spec.seconds) * 300);
    std::atomic<bool> ended{false};
    std::string endReason;

    std::string error;
    std::unique_ptr<mw::native::Session> session = mw::native::NativeHost::createSession(
        config,
        [&](const mw::native::EncodedFrame& f) {
            BenchRow row;
            row.frameNumber = f.frameNumber;
            row.keyframe = f.keyframe;
            // A re-send stamps present, captured and submitted with the same
            // "now" — see WindowsSession::emit. A capture never does.
            row.captured = !(f.presentUs == f.submittedUs && f.capturedUs == f.submittedUs);
            row.bytes = f.size;
            row.avgQp = f.avgQp;
            row.presentUs = f.presentUs;
            row.capturedUs = f.capturedUs;
            row.submittedUs = f.submittedUs;
            row.convertedUs = f.convertedUs;
            row.dueUs = f.dueUs;
            row.encodedUs = f.encodedUs;
            std::lock_guard<std::mutex> lock(rowsMutex);
            rows.push_back(row);
        },
        nullptr, nullptr, nullptr,
        [&](const std::string& reason) {
            endReason = reason;
            ended.store(true);
        },
        error);
    if (!session) {
        err << "native-bench: could not create the session: " << QString::fromStdString(error)
            << "\n";
        err.flush();
        return 1;
    }
    if (!session->start(error)) {
        err << "native-bench: could not start the session: " << QString::fromStdString(error)
            << "\n";
        err.flush();
        return 1;
    }

    const mw::native::SessionInfo& info = session->info();
    out << "native-bench: " << QString::fromStdString(info.gpuName) << " · "
        << mw::native::toString(info.encoder) << " " << mw::native::toString(info.codec)
        << (info.yuv444 ? " 4:4:4" : " 4:2:0") << " · " << info.width << "x" << info.height
        << " · fps " << (info.fps > 0 ? QString::number(info.fps) : QString("display")) << " · "
        << spec.bitrateKbps << " kbps" << (info.intraRefresh ? " · intra-refresh" : "") << " · "
        << spec.seconds << " s on " << QString::fromStdString(display->label) << "\n";
    out.flush();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(spec.seconds);
    while (std::chrono::steady_clock::now() < deadline && !ended.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session->stop();
    session.reset();
    if (ended.load())
        err << "native-bench: the session ended early: " << QString::fromStdString(endReason)
            << "\n";

    // ── CSV, one row per frame ──────────────────────────────────────────────
    QString path = spec.out;
    if (path.isEmpty())
        path = QDir::current().filePath(
            "native-bench-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".csv");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err << "native-bench: cannot write " << path << ": " << file.errorString() << "\n";
        err.flush();
        return 1;
    }
    {
        QTextStream csv(&file);
        csv << "frame,keyframe,captured,bytes,avg_qp,t0_present_us,t1_captured_us,"
               "t1b_submitted_us,t2_converted_us,t2b_due_us,t3_encoded_us,acquire_us,convert_us,"
               "hold_us,encode_us,host_total_us\n";
        for (const BenchRow& r : rows) {
            csv << r.frameNumber << ',' << (r.keyframe ? 1 : 0) << ',' << (r.captured ? 1 : 0)
                << ',' << static_cast<qulonglong>(r.bytes) << ',' << r.avgQp << ',' << r.presentUs
                << ',' << r.capturedUs << ',' << r.submittedUs << ',' << r.convertedUs << ','
                << r.dueUs << ',' << r.encodedUs << ',' << (r.capturedUs - r.presentUs) << ','
                << (r.convertedUs - r.submittedUs) << ',' << (r.dueUs - r.convertedUs) << ','
                << (r.encodedUs - r.dueUs) << ',' << (r.encodedUs - r.presentUs) << '\n';
        }
    }
    file.close();

    // ── Summary ─────────────────────────────────────────────────────────────
    // Captured frames only for the stage figures: a re-send has no capture and
    // would read as a 0 µs acquire. Bytes and QP over everything the encoder
    // produced, keyframes and re-sends included — that is what goes on the wire.
    mw::native::StageStats stages;
    mw::native::LatencyHistogram bytesAll, bytesDelta, qp;
    int captured = 0, keyframes = 0, resends = 0;
    int64_t firstUs = 0, lastUs = 0;
    for (const BenchRow& r : rows) {
        if (r.keyframe) keyframes++;
        bytesAll.add(static_cast<int64_t>(r.bytes));
        if (!r.keyframe) bytesDelta.add(static_cast<int64_t>(r.bytes));
        if (r.avgQp >= 0) qp.add(r.avgQp);
        if (!r.captured) {
            resends++;
            continue;
        }
        captured++;
        if (firstUs == 0) firstUs = r.presentUs;
        lastUs = r.presentUs;
        stages.record(mw::native::Stage::Acquire, r.capturedUs - r.presentUs);
        stages.record(mw::native::Stage::Convert, r.convertedUs - r.submittedUs);
        stages.record(mw::native::Stage::Hold, r.dueUs - r.convertedUs);
        stages.record(mw::native::Stage::Encode, r.encodedUs - r.dueUs);
        stages.record(mw::native::Stage::Total, r.encodedUs - r.presentUs);
    }
    const double spanS = (lastUs > firstUs) ? (lastUs - firstUs) / 1e6 : 0.0;
    const auto s = stages.session();
    const auto st = [&](mw::native::Stage stage) {
        const mw::native::StageSummary& x = s[static_cast<size_t>(stage)];
        if (x.count == 0) return QString("-");
        return QString("%1 / %2 / %3")
            .arg(x.meanUs / 1000.0, 0, 'f', 2)
            .arg(x.p95Us / 1000.0, 0, 'f', 2)
            .arg(x.p99Us / 1000.0, 0, 'f', 2);
    };

    out << "frames          " << rows.size() << " (" << captured << " captured, " << resends
        << " re-sent, " << keyframes << " keyframes)\n";
    if (spanS > 0)
        out << "capture rate    " << QString::number((captured - 1) / spanS, 'f', 1) << " fps over "
            << QString::number(spanS, 'f', 1) << " s\n";
    out << "                mean / p95 / p99\n";
    out << "acquire   ms    " << st(mw::native::Stage::Acquire) << "\n";
    out << "convert   ms    " << st(mw::native::Stage::Convert) << "\n";
    out << "hold      ms    " << st(mw::native::Stage::Hold) << "\n";
    out << "encode    ms    " << st(mw::native::Stage::Encode) << "\n";
    out << "present→encoded " << st(mw::native::Stage::Total) << "\n";
    out << "bytes/frame KB  " << tail(bytesAll, 1024.0, 1) << "  (deltas "
        << tail(bytesDelta, 1024.0, 1) << ")\n";
    out << "avg QP          " << tail(qp, 1.0, 1) << (qp.count() == 0 ? "  (not reported)" : "")
        << "\n";
    out << "csv             " << QDir::toNativeSeparators(path) << "\n";
    out.flush();
    return 0;
}
