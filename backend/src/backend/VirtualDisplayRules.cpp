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

#include "backend/VirtualDisplay.h"

#include "common/Edition.h"

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>

// The platform-free half of VirtualDisplay: names, the request/result files,
// the driver's settings XML. Kept apart from VirtualDisplay.cpp so the
// security test target can link it without SetupAPI, the probe service or a
// driver — these are the rules that decide what an elevated process will do,
// and they are verified on every platform (tests/test_virtual_display.cpp).

namespace VirtualDisplay {

QString displayNameFor(const QString& productName)
{
    return productName + QStringLiteral(" Virtual Display");
}

QString displayName()
{
    // The installed edition's name, dev flag or not: a --dev instance is a
    // scratch copy of the same edition and streams the display the installer
    // of that edition created.
    return displayNameFor(mw::edition::productName());
}

QString taskNameFor(const QString& productName, bool devFlag)
{
    return productName + (devFlag ? QStringLiteral("-dev") : QString()) +
           QStringLiteral(" Virtual Display");
}

QString taskName()
{
    return taskNameFor(mw::edition::productName(), mw::edition::devFlag());
}

QString toString(Request::Action action)
{
    switch (action) {
    case Request::Action::Install: return QStringLiteral("install");
    case Request::Action::Uninstall: return QStringLiteral("uninstall");
    case Request::Action::Activate: return QStringLiteral("activate");
    case Request::Action::Deactivate: return QStringLiteral("deactivate");
    case Request::Action::OthersBack: return QStringLiteral("others_back");
    }
    return QString();
}

std::optional<Request> parseRequest(const QByteArray& json, QString* error)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("request is not a JSON object");
        return std::nullopt;
    }
    const QJsonObject obj = doc.object();
    Request req;
    const QString action = obj.value(QLatin1String("action")).toString();
    bool known = false;
    for (const Request::Action a :
         {Request::Action::Install, Request::Action::Uninstall, Request::Action::Activate,
          Request::Action::Deactivate, Request::Action::OthersBack}) {
        if (action == toString(a)) {
            req.action = a;
            known = true;
        }
    }
    if (!known) {
        if (error) *error = QStringLiteral("unknown action");
        return std::nullopt;
    }
    const QJsonValue restore = obj.value(QLatin1String("restore_primary"));
    if (!restore.isUndefined() && !restore.isNull() && !restore.isString()) {
        if (error) *error = QStringLiteral("restore_primary must be a string");
        return std::nullopt;
    }
    req.restorePrimary = restore.toString().trimmed();
    if (req.restorePrimary.size() > 512) {
        if (error) *error = QStringLiteral("restore_primary too long");
        return std::nullopt;
    }
    for (const QChar c : req.restorePrimary) {
        // A display key is compared, never interpreted — but an elevated
        // process reads it, and a control character has no place in one.
        if (c.category() == QChar::Other_Control) {
            if (error) *error = QStringLiteral("restore_primary contains control characters");
            return std::nullopt;
        }
    }
    // The size the client asks for. Out of bounds is pinned, not refused: a
    // launch must never fail over it, and the default mode is always there.
    req.width = obj.value(QLatin1String("width")).toInt(0);
    req.height = obj.value(QLatin1String("height")).toInt(0);
    normaliseMode(req.width, req.height);
    // The rate in millihertz; an older server's whole hertz otherwise.
    const QJsonValue milli = obj.value(QLatin1String("refresh_mhz"));
    req.refreshMilliHz = milli.isUndefined()
                             ? obj.value(QLatin1String("refresh")).toInt(0) * kMilliHz
                             : milli.toInt(0);
    normaliseRate(req.refreshMilliHz);
    const QJsonValue exclusive = obj.value(QLatin1String("exclusive"));
    if (!exclusive.isUndefined() && !exclusive.isBool()) {
        if (error) *error = QStringLiteral("exclusive must be a boolean");
        return std::nullopt;
    }
    req.exclusive = req.action == Request::Action::Activate && exclusive.toBool(false);
    const QJsonValue gpu = obj.value(QLatin1String("gpu"));
    if (!gpu.isUndefined() && !gpu.isNull()) {
        if (!gpu.isString() || !validGpuName(gpu.toString())) {
            if (error) *error = QStringLiteral("gpu is not a GPU name");
            return std::nullopt;
        }
        if (req.action == Request::Action::Activate) req.gpu = gpu.toString();
    }
    const QJsonValue benchHdr = obj.value(QLatin1String("bench_hdr"));
    if (!benchHdr.isUndefined() && !benchHdr.isBool()) {
        if (error) *error = QStringLiteral("bench_hdr must be a boolean");
        return std::nullopt;
    }
    req.benchHdr = req.action == Request::Action::Activate && benchHdr.toBool(false);
    const QJsonValue benchSecondary = obj.value(QLatin1String("bench_secondary"));
    if (!benchSecondary.isUndefined() && !benchSecondary.isBool()) {
        if (error) *error = QStringLiteral("bench_secondary must be a boolean");
        return std::nullopt;
    }
    req.benchSecondary = req.action == Request::Action::Activate && benchSecondary.toBool(false);
    return req;
}

QString gpuOfSettings(const QString& xml)
{
    static const QRegularExpression re(
        QStringLiteral("<gpu>\\s*<friendlyname>([^<]*)</friendlyname>"));
    const QRegularExpressionMatch m = re.match(xml);
    if (!m.hasMatch()) return QString();
    const QString name = m.captured(1).trimmed();
    return name.compare(QLatin1String("default"), Qt::CaseInsensitive) == 0 ? QString() : name;
}

bool othersGoOff(Asker asker, bool optionOn, bool guestOn, bool othersOff)
{
    return asker == Asker::Owner && optionOn && (!guestOn || othersOff);
}

bool ownerWantsOthersOff(bool allowed, bool hasChoice, bool choice, bool inherited)
{
    return allowed && (hasChoice ? choice : inherited);
}

QList<int> darkDisplays(const QList<int>& lit, const QList<int>& listed, bool othersOff)
{
    QList<int> dark;
    if (!othersOff) return dark;
    for (int id : lit)
        if (!listed.contains(id) && !dark.contains(id)) dark.append(id);
    return dark;
}

bool othersComeBack(bool ownerOn, bool othersOff)
{
    return othersOff && !ownerOn;
}

bool savesLayout(LayoutStep step, bool othersOff)
{
    switch (step) {
    case LayoutStep::Impose: return true;
    case LayoutStep::Prune:
    case LayoutStep::Mode:
    case LayoutStep::Primary: return !othersOff;
    case LayoutStep::Exclusive:
    case LayoutStep::Restore: return false;
    }
    return false;
}

GuardStep guardStep(bool serverAlive, bool othersOff)
{
    if (!othersOff) return GuardStep::Leave;
    return serverAlive ? GuardStep::Wait : GuardStep::Restore;
}

bool othersOffAfter(LayoutStep step, bool othersOff, bool ok)
{
    if (!ok) return othersOff;
    switch (step) {
    case LayoutStep::Impose:
    case LayoutStep::Restore: return false;
    case LayoutStep::Exclusive: return true;
    default: return othersOff;
    }
}

QString notAppearedError()
{
    return QStringLiteral("the virtual display did not appear");
}

std::optional<Request> fallbackTowardsDefault(const Request& tried, const QString& error)
{
    if (tried.action != Request::Action::Activate || error != notAppearedError())
        return std::nullopt;
    Request next = tried;
    if (next.width > 0 || next.height > 0) {
        next.width = next.height = 0;
        return next;
    }
    if (next.refreshMilliHz > 0 && next.refreshMilliHz != kRefreshHz * kMilliHz) {
        next.refreshMilliHz = 0;
        return next;
    }
    return std::nullopt;
}

QByteArray toJson(const Request& req)
{
    QJsonObject obj;
    obj["action"] = toString(req.action);
    if (!req.restorePrimary.isEmpty()) obj["restore_primary"] = req.restorePrimary;
    if (req.width > 0 && req.height > 0) {
        obj["width"] = req.width;
        obj["height"] = req.height;
    }
    if (req.refreshMilliHz > 0) obj["refresh_mhz"] = req.refreshMilliHz;
    if (req.exclusive) obj["exclusive"] = true;
    if (!req.gpu.isEmpty()) obj["gpu"] = req.gpu;
    if (req.benchHdr) obj["bench_hdr"] = true;
    if (req.benchSecondary) obj["bench_secondary"] = true;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool normaliseMode(int& width, int& height)
{
    if (width <= 0 || height <= 0) {
        width = height = 0;
        return false;
    }
    const auto pin = [](int v) { return std::min(kModeMax, std::max(kModeMin, v)) & ~1; };
    width = pin(width);
    height = pin(height);
    return true;
}

bool normaliseRate(int& milliHz)
{
    if (milliHz <= 0) {
        milliHz = 0;
        return false;
    }
    milliHz = std::min(kRateDriverMax * kMilliHz, std::max(kRateMin * kMilliHz, milliHz));
    return true;
}

QString rateText(int milliHz)
{
    QString text = QString::number(milliHz / kMilliHz);
    if (const int frac = milliHz % kMilliHz; frac > 0) {
        QString digits = QStringLiteral("%1").arg(frac, 3, 10, QLatin1Char('0'));
        while (digits.endsWith(QLatin1Char('0')))
            digits.chop(1);
        text += QLatin1Char('.') + digits;
    }
    return text;
}

RateFraction rateFraction(int milliHz)
{
    if (milliHz <= 0) return {};
    unsigned a = unsigned(milliHz), b = unsigned(kMilliHz);
    while (b != 0) {
        const unsigned t = a % b;
        a = b;
        b = t;
    }
    return {unsigned(milliHz) / a, unsigned(kMilliHz) / a};
}

int matchedRefreshMilliHz(int clientMilliHz, int streamFps, int capHz)
{
    if (clientMilliHz < kRateMin * kMilliHz || streamFps <= 0 || capHz <= 0) return 0;
    capHz = std::min(capHz, kRateDriverMax);
    // How many of the client's refreshes a frame of the stream spans: the
    // stream must be the client's rate or a whole divisor of it, to the
    // gate's tolerance — or no display rate puts its frames on that grid.
    const qint64 streamMilli = qint64(streamFps) * kMilliHz;
    const qint64 divisor = (clientMilliHz + streamMilli / 2) / streamMilli;
    if (divisor < 1) return 0;
    const qint64 off = qint64(clientMilliHz) - divisor * streamMilli;
    if ((off < 0 ? -off : off) * 1000 > qint64(clientMilliHz) * 2) return 0;
    for (qint64 k = qint64(capHz) * kMilliHz / clientMilliHz; k >= 1; --k) {
        // The display's presents per frame of the stream: 1, or even, so
        // that "Auto"'s step to twice the rate still divides the display.
        const qint64 perFrame = k * divisor;
        if (perFrame != 1 && perFrame % 2 != 0) continue;
        const qint64 milliHz = k * clientMilliHz;
        if (milliHz < qint64(kRateMin) * kMilliHz) break;
        return int(milliHz);
    }
    return 0;
}

qint64 refusalHoldMs(int count)
{
    constexpr qint64 kFirstMs = 12LL * 3600 * 1000;
    constexpr qint64 kMaxMs = 14LL * 24 * 3600 * 1000;
    const int doublings = std::clamp(count, 1, 16) - 1;
    return std::min(kMaxMs, kFirstMs << doublings);
}

int matchCapAfterRefusals(int capHz, const QJsonArray& refused, const QString& version,
                          const QStringList& gpus, qint64 nowMs)
{
    int cap = capHz;
    for (const QJsonValue& v : refused) {
        const QJsonObject o = v.toObject();
        if (o.value(QLatin1String("version")).toString() != version) continue;
        if (!gpus.isEmpty() && !gpus.contains(o.value(QLatin1String("gpu")).toString())) continue;
        // Its hold over: the next stream tries the rate again. A clock set
        // back puts the refusal in the future; trying again is the cheap side.
        const qint64 at = o.value(QLatin1String("at")).toInteger(0);
        const int count = static_cast<int>(o.value(QLatin1String("count")).toInteger(1));
        if (at <= 0 || at > nowMs || nowMs - at >= refusalHoldMs(count)) continue;
        const qint64 milliHz = o.value(QLatin1String("milli_hz")).toInteger();
        if (milliHz <= 0) continue;
        // 2 % under it, whole hertz: a screen a little slower than the one
        // refused would put the display back at the same load.
        const int under = static_cast<int>(milliHz * 49 / 50 / kMilliHz);
        cap = std::min(cap, under);
    }
    return cap;
}

int refreshForStream(int streamFps, int benchHz, bool faster)
{
    if (benchHz > 0) return std::min(kRateDriverMax, std::max(kRateMin, benchHz));
    if (faster) return kRateMax;
    if (streamFps <= 0) return 0;
    return std::min(kRateMax, std::max(kRateMin, streamFps));
}

ModeSize sizeForStream(int width, int height, bool match)
{
    if (width <= 0 || height <= 0) return {};
    if (match || qint64(width) * height > qint64(kWidth) * kHeight) return {width, height};
    return {};
}

std::optional<Result> parseResult(const QByteArray& json)
{
    if (json.trimmed().isEmpty()) return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject obj = doc.object();
    if (!obj.contains(QLatin1String("ok"))) return std::nullopt;
    Result res;
    res.ok = obj.value(QLatin1String("ok")).toBool(false);
    res.stage = obj.value(QLatin1String("stage")).toString();
    res.error = obj.value(QLatin1String("error")).toString();
    res.rebootRequired = obj.value(QLatin1String("reboot_required")).toBool(false);
    res.display = obj.value(QLatin1String("display")).toString();
    res.previousPrimary = obj.value(QLatin1String("previous_primary")).toString();
    return res;
}

std::optional<Result> readResult(const QString& path)
{
    QByteArray json;
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
        json = f.readAll();
    }
    return parseResult(json);
}

QByteArray toJson(const Result& res)
{
    QJsonObject obj;
    obj["ok"] = res.ok;
    obj["stage"] = res.stage;
    if (!res.error.isEmpty()) obj["error"] = res.error;
    obj["reboot_required"] = res.rebootRequired;
    if (!res.display.isEmpty()) obj["display"] = res.display;
    if (!res.previousPrimary.isEmpty()) obj["previous_primary"] = res.previousPrimary;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

/// The line that makes the settings file ours. A VDD the owner installed
/// before us wrote a file without it, and that file is never touched.
static QString settingsMarker()
{
    return QStringLiteral("<!-- written by MoonlightWeb — do not edit, it is rewritten -->");
}

bool isOurSettings(const QString& xml)
{
    return xml.contains(settingsMarker());
}

/// One `<resolution>` block in the driver's own spelling: one refresh rate,
/// the one this activation wants, exactly as the file we write ourselves
/// spells it — the shape the driver was seen to accept on the bench, and the
/// only one to put in a file belonging to someone else.
static QString resolutionBlock(int width, int height, int refreshMilliHz, const QString& indent)
{
    QString s;
    s += indent + QStringLiteral("<resolution>\n");
    s += indent + QStringLiteral("  <width>%1</width>\n").arg(width);
    s += indent + QStringLiteral("  <height>%1</height>\n").arg(height);
    s += indent +
         QStringLiteral("  <refresh_rate>%1</refresh_rate>\n").arg(rateText(refreshMilliHz));
    s += indent + QStringLiteral("</resolution>\n");
    return s;
}

QString settingsWithMode(const QString& existing, int width, int height, int refreshMilliHz,
                         bool* changed)
{
    *changed = false;
    if (!normaliseMode(width, height)) return existing;
    if (!normaliseRate(refreshMilliHz)) refreshMilliHz = kRefreshHz * kMilliHz;
    // Already offered: the driver lists this size AT this rate, nothing to
    // add. All three tags in the same block is what makes it this mode rather
    // than others that happen to share a number — and a size listed at
    // another rate still needs its own entry, since the rate is half of what
    // the client asked for.
    static const QRegularExpression block(QStringLiteral("<resolution>.*?</resolution>"),
                                          QRegularExpression::DotMatchesEverythingOption);
    const QString w = QStringLiteral("<width>%1</width>").arg(width);
    const QString h = QStringLiteral("<height>%1</height>").arg(height);
    const QString r =
        QStringLiteral("<refresh_rate>%1</refresh_rate>").arg(rateText(refreshMilliHz));
    auto it = block.globalMatch(existing);
    while (it.hasNext()) {
        const QString one =
            it.next().captured(0).remove(QLatin1Char(' ')).remove(QLatin1Char('\t'));
        if (one.contains(w) && one.contains(h) && one.contains(r)) return existing;
    }
    const int open = existing.indexOf(QStringLiteral("<resolutions>"));
    if (open < 0) return existing;
    const int after = open + int(strlen("<resolutions>"));
    // The list is read in order and the first entry is the driver's default:
    // ours goes first, which is the point of adding it at all.
    QString out = existing;
    out.insert(after, QLatin1Char('\n') +
                          resolutionBlock(width, height, refreshMilliHz, QStringLiteral("    ")));
    *changed = true;
    return out;
}

QString settingsWithoutMode(const QString& existing, int width, int height, int refreshMilliHz,
                            bool* changed)
{
    *changed = false;
    if (width <= 0 || height <= 0 || refreshMilliHz <= 0) return existing;
    // As settingsWithMode() put it in: taking out these very characters gives
    // the owner the file they had.
    const QString inserted =
        QLatin1Char('\n') + resolutionBlock(width, height, refreshMilliHz, QStringLiteral("    "));
    QString out = existing;
    const int at = out.indexOf(inserted);
    if (at >= 0) {
        out.remove(at, inserted.size());
        *changed = true;
        return out;
    }
    // Someone reformatted the file since: the first block listing the three
    // values, with the blanks around it on its own lines.
    static const QRegularExpression block(
        QStringLiteral("[ \\t]*<resolution>.*?</resolution>[ \\t]*\\n?"),
        QRegularExpression::DotMatchesEverythingOption);
    const QString w = QStringLiteral("<width>%1</width>").arg(width);
    const QString h = QStringLiteral("<height>%1</height>").arg(height);
    const QString r =
        QStringLiteral("<refresh_rate>%1</refresh_rate>").arg(rateText(refreshMilliHz));
    auto it = block.globalMatch(existing);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString one = m.captured(0).remove(QLatin1Char(' ')).remove(QLatin1Char('\t'));
        if (one.contains(w) && one.contains(h) && one.contains(r)) {
            out.remove(m.capturedStart(0), m.capturedLength(0));
            *changed = true;
            return out;
        }
    }
    return existing;
}

QList<AddedMode> parseAddedModes(const QString& text)
{
    // The rate as rateText() spells it: whole hertz (every record written
    // before millihertz), or up to three decimals.
    static const QRegularExpression line(
        QStringLiteral("^\\s*(\\d+)x(\\d+)@(\\d+)(?:\\.(\\d{1,3}))?\\s*$"));
    QList<AddedMode> out;
    for (const QString& l : text.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = line.match(l);
        if (!m.hasMatch()) continue;
        const int milli =
            m.captured(4).isEmpty() ? 0 : m.captured(4).leftJustified(3, QLatin1Char('0')).toInt();
        AddedMode mode{m.captured(1).toInt(), m.captured(2).toInt(),
                       m.captured(3).toInt() * kMilliHz + milli};
        if (mode.width > 0 && mode.height > 0 && mode.refreshMilliHz > 0 && !out.contains(mode))
            out.append(mode);
    }
    return out;
}

QString addedModesText(const QList<AddedMode>& modes)
{
    QString out;
    for (const AddedMode& m : modes)
        out +=
            QStringLiteral("%1x%2@%3\n").arg(m.width).arg(m.height).arg(rateText(m.refreshMilliHz));
    return out;
}

QString settingsXml(int width, int height, int refreshMilliHz, const QString& gpu)
{
    // The layout the driver ships (its own sample, verified on the bench):
    //   <monitors><count>, <gpu><friendlyname>, <global><g_refresh_rate>*,
    //   <resolutions><resolution>{width,height,refresh_rate}*, <options>.
    // Our mode goes first — the driver takes the first as the default — then
    // the common sizes, so a later mode change needs no driver reload.
    // A client size comes ahead of all of them: it is the mode this
    // activation exists for, and the desktop must come up at it. Every size
    // carries the client's own refresh rate, since that is the cadence the
    // frames this display exists to produce will be shown at.
    struct Size
    {
        int w, h;
    };
    static const Size sizes[] = {{1280, 720}, {2560, 1440}, {3840, 2160}};
    static const int rates[] = {60 * kMilliHz, 90 * kMilliHz, 144 * kMilliHz};
    const bool custom = normaliseMode(width, height);
    int refresh = refreshMilliHz;
    if (!normaliseRate(refresh)) refresh = kRefreshHz * kMilliHz;

    QString xml;
    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml += settingsMarker() + QLatin1Char('\n');
    xml += QStringLiteral("<vdd_settings>\n");
    xml += QStringLiteral("  <monitors>\n    <count>1</count>\n  </monitors>\n");
    // The GPU that renders the display: the one the owner chose on the
    // display's card (Request::gpu), else the driver's own choice — unless the
    // bench names one. MW_VDD_GPU is how a GPU with no screen of its own (the
    // RTX of DualRTX) gets a display to capture, so its encoder can be measured
    // in a real stream rather than headless. Never set by the product.
    QString renderer = qEnvironmentVariable("MW_VDD_GPU");
    if (renderer.isEmpty()) renderer = validGpuName(gpu) ? gpu : QStringLiteral("default");
    xml += QStringLiteral("  <gpu>\n    <friendlyname>%1</friendlyname>\n  </gpu>\n").arg(renderer);
    xml += QStringLiteral("  <global>\n");
    xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(rateText(refresh));
    for (const int milli : rates)
        if (milli != refresh)
            xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(rateText(milli));
    if (refresh != kRefreshHz * kMilliHz)
        xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(kRefreshHz);
    xml += QStringLiteral("  </global>\n  <resolutions>\n");
    const auto one = [&xml, refresh](int w, int h) {
        xml += QStringLiteral("    <resolution>\n      <width>%1</width>\n"
                              "      <height>%2</height>\n      <refresh_rate>%3</refresh_rate>\n"
                              "    </resolution>\n")
                   .arg(w)
                   .arg(h)
                   .arg(rateText(refresh));
    };
    if (custom) one(width, height);
    if (!custom || width != kWidth || height != kHeight) one(kWidth, kHeight);
    for (const Size& s : sizes)
        if (!custom || s.w != width || s.h != height) one(s.w, s.h);
    xml += QStringLiteral("  </resolutions>\n  <options>\n");
    xml += QStringLiteral("    <CustomEdid>false</CustomEdid>\n"
                          "    <PreventSpoof>false</PreventSpoof>\n"
                          "    <EdidCeaOverride>false</EdidCeaOverride>\n"
                          "    <HardwareCursor>true</HardwareCursor>\n"
                          "    <SDR10bit>false</SDR10bit>\n    <HDRPlus>false</HDRPlus>\n"
                          "    <logging>false</logging>\n    <debuglogging>false</debuglogging>\n"
                          "  </options>\n</vdd_settings>\n");
    return xml;
}

QPoint placeBeside(const QList<DesktopRect>& others)
{
    const DesktopRect* edge = nullptr;
    for (const DesktopRect& r : others) {
        if (r.width <= 0 || r.height <= 0) continue;
        const int right = r.x + r.width;
        const int edgeRight = edge ? edge->x + edge->width : 0;
        if (!edge || right > edgeRight || (right == edgeRight && r.y < edge->y)) edge = &r;
    }
    return edge ? QPoint(edge->x + edge->width, edge->y) : QPoint(0, 0);
}

} // namespace VirtualDisplay
