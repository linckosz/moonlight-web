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
#include "test_framework.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <iterator>

// The pure half of "MoonlightWeb Virtual Display": what an elevated helper
// will act on (the request file), what it answers (the result file), what it
// writes for the driver, and the names. No driver, no socket, every platform.

namespace {

bool isHex(const QString& s, int len)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]+$"));
    return s.size() == len && hex.match(s).hasMatch();
}

} // namespace

void run_virtual_display_tests()
{
    using namespace VirtualDisplay;

    SECTION("VirtualDisplay — one name per edition, for the card, the node and the task");
    CHECK_EQ(displayNameFor(QStringLiteral("MoonlightWeb")),
             QStringLiteral("MoonlightWeb Virtual Display"));
    CHECK_EQ(displayNameFor(QStringLiteral("MoonlightWebDev")),
             QStringLiteral("MoonlightWebDev Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWeb"), false),
             QStringLiteral("MoonlightWeb Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWebDev"), false),
             QStringLiteral("MoonlightWebDev Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWeb"), true),
             QStringLiteral("MoonlightWeb-dev Virtual Display"));
    // The installer's task and the device node share the edition's name: the
    // --dev flag names the task only (a --dev instance never installs).
    CHECK_EQ(displayNameFor(QStringLiteral("MoonlightWeb")),
             taskNameFor(QStringLiteral("MoonlightWeb"), false));
    CHECK_EQ(applyArgument(), QStringLiteral("--vdisplay-apply"));

    SECTION("VirtualDisplay — the mode is 1080p at 120 Hz, SDR");
    CHECK_EQ(kWidth, 1920);
    CHECK_EQ(kHeight, 1080);
    CHECK_EQ(kRefreshHz, 120);

    SECTION("VirtualDisplay — only the five verbs get through the request parser");
    QString err;
    for (const char* verb : {"install", "uninstall", "activate", "deactivate", "others_back"}) {
        const auto req = parseRequest(QByteArray("{\"action\":\"") + verb + "\"}", &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(toString(req->action), QString::fromLatin1(verb));
    }
    CHECK(!parseRequest("{\"action\":\"add\"}", &err).has_value());
    CHECK(!parseRequest("{\"action\":\"format-c\"}", &err).has_value());
    CHECK(!parseRequest("{}", &err).has_value());
    CHECK(!parseRequest("not json", &err).has_value());
    CHECK(!parseRequest("[1,2]", &err).has_value());
    // The one parameter: a display key to give the primary role back to,
    // compared and never interpreted — but read by an elevated process.
    {
        const auto req = parseRequest(
            "{\"action\":\"deactivate\",\"restore_primary\":\"\\\\\\\\?\\\\DISPLAY#GBT270D#5&1\"}",
            &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(req->restorePrimary, QStringLiteral("\\\\?\\DISPLAY#GBT270D#5&1"));
    }
    CHECK(!parseRequest("{\"action\":\"deactivate\",\"restore_primary\":42}", &err).has_value());
    CHECK(!parseRequest("{\"action\":\"deactivate\",\"restore_primary\":\"a\\nb\"}", &err)
               .has_value());
    {
        const QByteArray longKey = QByteArray(600, 'x');
        CHECK(!parseRequest("{\"action\":\"deactivate\",\"restore_primary\":\"" + longKey + "\"}",
                            &err)
                   .has_value());
    }
    // Round trip.
    {
        Request r;
        r.action = Request::Action::Deactivate;
        r.restorePrimary = QStringLiteral("12345");
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) {
            CHECK(back->action == Request::Action::Deactivate);
            CHECK_EQ(back->restorePrimary, QStringLiteral("12345"));
        }
        Request a;
        a.action = Request::Action::Activate;
        CHECK(!toJson(a).contains("restore_primary"));
    }

    SECTION("VirtualDisplay — the result file is read only once complete");
    CHECK(!parseResult("").has_value());
    CHECK(!parseResult("{\"st").has_value());
    CHECK(!parseResult("{}").has_value());
    {
        Result r;
        r.ok = true;
        r.stage = QStringLiteral("done");
        r.rebootRequired = true;
        r.display = QStringLiteral("\\\\.\\DISPLAY3");
        r.previousPrimary = QStringLiteral("\\\\?\\DISPLAY#GBT270D#5&1");
        const auto back = parseResult(toJson(r));
        CHECK(back.has_value());
        if (back) {
            CHECK(back->ok);
            CHECK(back->rebootRequired);
            CHECK_EQ(back->display, QStringLiteral("\\\\.\\DISPLAY3"));
            CHECK_EQ(back->previousPrimary, QStringLiteral("\\\\?\\DISPLAY#GBT270D#5&1"));
        }
        // One line, so a reader taking the last stdout line gets all of it.
        CHECK_EQ(toJson(r).count('\n'), 1);
    }

    SECTION("VirtualDisplay — a result file read is let go of, so the next run can remove it");
    {
        // DualRTX, 13/10/2026: the file stayed open while its failure was
        // acted on, the fallback could not remove it, and its next run was
        // answered by that same failure — /start 500 while the display came up.
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("result.json"));
        CHECK(!readResult(path).has_value());
        Result failed;
        failed.stage = QStringLiteral("mode");
        failed.error = notAppearedError();
        {
            QFile f(path);
            CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(toJson(failed));
        }
        const auto read = readResult(path);
        CHECK(read.has_value());
        if (read) {
            CHECK(!read->ok);
            CHECK_EQ(read->error, notAppearedError());
        }
        CHECK(QFile::remove(path));
        CHECK(!readResult(path).has_value());
        // Half written: nothing yet, and still not held.
        {
            QFile f(path);
            CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("{\"ok\":fal");
        }
        CHECK(!readResult(path).has_value());
        CHECK(QFile::remove(path));
    }

    SECTION("VirtualDisplay — the driver settings put our mode first, SDR, default GPU");
    {
        const QString xml = settingsXml();
        CHECK(xml.contains(QStringLiteral("<count>1</count>")));
        CHECK(xml.contains(QStringLiteral("<friendlyname>default</friendlyname>")));
        CHECK(xml.contains(QStringLiteral("<HDRPlus>false</HDRPlus>")));
        CHECK(xml.contains(QStringLiteral("<SDR10bit>false</SDR10bit>")));
        const int firstRes = xml.indexOf(QStringLiteral("<resolution>"));
        const int ours = xml.indexOf(QStringLiteral("<width>1920</width>"));
        CHECK(firstRes >= 0 && ours > firstRes);
        CHECK(xml.indexOf(QStringLiteral("<width>2560</width>")) > ours);
        CHECK(xml.indexOf(QStringLiteral("<g_refresh_rate>120</g_refresh_rate>")) <
              xml.indexOf(QStringLiteral("<g_refresh_rate>60</g_refresh_rate>")));
    }

    SECTION("VirtualDisplay — a client size is pinned, made even, or is no size at all");
    {
        int w = 2532, h = 1170;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, 2532);
        CHECK_EQ(h, 1170);
        // Odd is rounded down: a display mode is whole macroblocks to the
        // encoder that follows it.
        w = 1365;
        h = 767;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, 1364);
        CHECK_EQ(h, 766);
        // A small desktop passes as it is: 480 lines is the floor, and an
        // 800×600 client is streamed 800×600, not stretched into a shape it
        // never asked for (the client already pinned the size at its own
        // shape — this is the guard behind it).
        w = 800;
        h = 600;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, 800);
        CHECK_EQ(h, 600);
        // Out of bounds is pinned, never refused: a launch must not fail here.
        w = 99999;
        h = 12;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, kModeMax);
        CHECK_EQ(h, kModeMin);
        // Nothing asked for: the default mode, and 0×0 says so.
        w = 0;
        h = 1080;
        CHECK(!normaliseMode(w, h));
        CHECK_EQ(w, 0);
        CHECK_EQ(h, 0);
        w = -1920;
        h = -1080;
        CHECK(!normaliseMode(w, h));
        CHECK_EQ(w, 0);
    }

    SECTION("VirtualDisplay — the size travels with the request and leads the mode list");
    {
        // "Match my screen" on a phone: the size reaches the elevated helper.
        const auto req =
            parseRequest("{\"action\":\"activate\",\"width\":2532,\"height\":1170}", &err);
        CHECK(req.has_value());
        if (req) {
            CHECK_EQ(req->width, 2532);
            CHECK_EQ(req->height, 1170);
        }
        // Junk and absence both mean "the mode that is there".
        const auto none = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(none.has_value());
        if (none) CHECK_EQ(none->width, 0);
        const auto bad =
            parseRequest("{\"action\":\"activate\",\"width\":\"big\",\"height\":8}", &err);
        CHECK(bad.has_value());
        if (bad) CHECK_EQ(bad->width, 0);
        Request r;
        r.action = Request::Action::Activate;
        r.width = 2532;
        r.height = 1170;
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) CHECK_EQ(back->height, 1170);
        CHECK(!toJson(Request{}).contains("width"));

        // The list the driver reads: the client's size first — it is the one
        // the desktop must come up at — then ours, then the common ones, each
        // listed once.
        const QString xml = settingsXml(2532, 1170);
        const int phone = xml.indexOf(QStringLiteral("<width>2532</width>"));
        CHECK(phone > 0);
        CHECK(xml.indexOf(QStringLiteral("<width>1920</width>")) > phone);
        CHECK(xml.contains(QStringLiteral("<height>1170</height>")));
        CHECK_EQ(settingsXml(1920, 1080).count(QStringLiteral("<width>1920</width>")), 1);
        CHECK_EQ(settingsXml(2560, 1440).count(QStringLiteral("<width>2560</width>")), 1);
        // A file we wrote is ours to rewrite; anyone else's VDD settings are
        // left exactly as they are.
        CHECK(isOurSettings(settingsXml()));
        CHECK(isOurSettings(settingsXml(2532, 1170)));
        CHECK(!isOurSettings(QStringLiteral("<?xml version=\"1.0\"?><vdd_settings/>")));
    }

    SECTION("VirtualDisplay — another VDD's mode list gains our size and keeps its own");
    {
        // A file the owner's own VDD wrote: their sizes, their refresh rates.
        const QString theirs =
            QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<vdd_settings>\n"
                           "  <resolutions>\n"
                           "    <resolution>\n      <width>800</width>\n"
                           "      <height>600</height>\n      <refresh_rate>30</refresh_rate>\n"
                           "    </resolution>\n"
                           "    <resolution>\n      <width>2560</width>\n"
                           "      <height>1440</height>\n      <refresh_rate>244</refresh_rate>\n"
                           "    </resolution>\n"
                           "  </resolutions>\n</vdd_settings>\n");
        bool changed = false;
        const QString grown = settingsWithMode(theirs, 2532, 1170, 120000, &changed);
        CHECK(changed);
        // Ours is there, first — the driver reads the list in order.
        const int mine = grown.indexOf(QStringLiteral("<width>2532</width>"));
        CHECK(mine > 0);
        CHECK(grown.indexOf(QStringLiteral("<width>800</width>")) > mine);
        // And every one of theirs survived, refresh rates included.
        CHECK(grown.contains(QStringLiteral("<refresh_rate>244</refresh_rate>")));
        CHECK(grown.contains(QStringLiteral("<height>600</height>")));
        CHECK(!isOurSettings(grown));
        // A size the file already lists AT THAT RATE is added again to
        // nobody.
        settingsWithMode(grown, 2532, 1170, 120000, &changed);
        CHECK(!changed);
        // The same size at another rate is another mode: the driver will not
        // offer a rate it does not list, and the next client's screen may
        // well run at 60 where this one ran at 120.
        const QString slower = settingsWithMode(grown, 2532, 1170, 60000, &changed);
        CHECK(changed);
        CHECK(slower.contains(QStringLiteral("<refresh_rate>60</refresh_rate>")));
        CHECK(slower.contains(QStringLiteral("<refresh_rate>244</refresh_rate>")));
        // Nothing to add, or nowhere to add it: the file comes back untouched.
        CHECK_EQ(settingsWithMode(theirs, 0, 0, 120000, &changed), theirs);
        CHECK(!changed);
        const QString noList = QStringLiteral("<vdd_settings><options/></vdd_settings>");
        CHECK_EQ(settingsWithMode(noList, 2532, 1170, 120000, &changed), noList);
        CHECK(!changed);
    }

    SECTION("VirtualDisplay — the mode added to another VDD's list comes out again");
    {
        const QString theirs =
            QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<vdd_settings>\n"
                           "  <resolutions>\n"
                           "    <resolution>\n      <width>800</width>\n"
                           "      <height>600</height>\n      <refresh_rate>30</refresh_rate>\n"
                           "    </resolution>\n"
                           "  </resolutions>\n</vdd_settings>\n");
        bool changed = false;
        const QString grown = settingsWithMode(theirs, 1784, 1160, 240000, &changed);
        CHECK(changed);
        // Out again: the owner's file, byte for byte.
        CHECK_EQ(settingsWithoutMode(grown, 1784, 1160, 240000, &changed), theirs);
        CHECK(changed);
        // Not there, or another rate of the same size: nothing touched.
        CHECK_EQ(settingsWithoutMode(theirs, 1784, 1160, 240000, &changed), theirs);
        CHECK(!changed);
        CHECK_EQ(settingsWithoutMode(grown, 1784, 1160, 120000, &changed), grown);
        CHECK(!changed);
        // Reformatted since: still found by its values, the owner's kept.
        QString tabbed = grown;
        tabbed.replace(QStringLiteral("    <resolution>\n      <width>1784"),
                       QStringLiteral("\t<resolution>\n\t\t<width>1784"));
        const QString back = settingsWithoutMode(tabbed, 1784, 1160, 240000, &changed);
        CHECK(changed);
        CHECK(!back.contains(QStringLiteral("<width>1784</width>")));
        CHECK(back.contains(QStringLiteral("<width>800</width>")));
        CHECK(back.contains(QStringLiteral("</resolutions>")));

        // DualRTX, 03/10/2026: a TV's 1280×720 at 240 left in the list, then a
        // Mac's 1784×1160 at 240. The next activation takes the first out
        // before it adds the second: one mode of ours at a time.
        const QString leftover = settingsWithMode(theirs, 1280, 720, 240000, &changed);
        const QString cleaned = settingsWithoutMode(leftover, 1280, 720, 240000, &changed);
        const QString next = settingsWithMode(cleaned, 1784, 1160, 240000, &changed);
        CHECK(!next.contains(QStringLiteral("<width>1280</width>")));
        CHECK(next.contains(QStringLiteral("<width>1784</width>")));
        CHECK_EQ(settingsWithoutMode(next, 1784, 1160, 240000, &changed), theirs);
    }

    SECTION("VirtualDisplay — the record of the modes we added reads back what it wrote");
    {
        const QList<AddedMode> modes = parseAddedModes(
            QStringLiteral("1784x1160@240\n1280x720@240\nnot a mode\n1784x1160@240\n0x0@60\n"));
        CHECK_EQ(int(modes.size()), 2);
        CHECK(modes.value(0) == (AddedMode{1784, 1160, 240000}));
        CHECK(modes.value(1) == (AddedMode{1280, 720, 240000}));
        CHECK_EQ(addedModesText(modes), QStringLiteral("1784x1160@240\n1280x720@240\n"));
        CHECK(parseAddedModes(QString()).isEmpty());
        // Beside the driver's own file, which both editions share.
        CHECK_EQ(QFileInfo(addedModesPath()).absolutePath(),
                 QFileInfo(settingsXmlPath()).absolutePath());
    }

    SECTION("VirtualDisplay — a display that does not come up falls back to the default mode");
    {
        // DSCP bench, 11/10/2026: a client's 1784x1160 at 240 Hz in the owner's
        // list and the display never appeared, so the launch failed. The size
        // goes first, then the rate, then nothing is left to drop.
        Request asked;
        asked.action = Request::Action::Activate;
        asked.width = 1784;
        asked.height = 1160;
        asked.refreshMilliHz = 240000;
        asked.restorePrimary = QStringLiteral("kept");
        const auto first = fallbackTowardsDefault(asked, notAppearedError());
        CHECK(first.has_value());
        if (first) {
            CHECK_EQ(first->width, 0);
            CHECK_EQ(first->height, 0);
            CHECK_EQ(first->refreshMilliHz, 240000);
            CHECK(first->action == Request::Action::Activate);
            CHECK_EQ(first->restorePrimary, QStringLiteral("kept"));
            const auto second = fallbackTowardsDefault(*first, notAppearedError());
            CHECK(second.has_value());
            if (second) {
                CHECK_EQ(second->width, 0);
                CHECK_EQ(second->refreshMilliHz, 0);
                CHECK(!fallbackTowardsDefault(*second, notAppearedError()).has_value());
            }
        }
        // The default rate asked explicitly: dropping it changes nothing.
        Request atDefaultRate;
        atDefaultRate.action = Request::Action::Activate;
        atDefaultRate.refreshMilliHz = kRefreshHz * kMilliHz;
        CHECK(!fallbackTowardsDefault(atDefaultRate, notAppearedError()).has_value());
        // A failure no mode explains is not retried, nor anything but Activate.
        CHECK(!fallbackTowardsDefault(asked, QStringLiteral("this process is not elevated"))
                   .has_value());
        Request off = asked;
        off.action = Request::Action::Deactivate;
        CHECK(!fallbackTowardsDefault(off, notAppearedError()).has_value());
    }

    SECTION("VirtualDisplay — the client's refresh rate travels and fills the mode list");
    {
        // Pinned into bounds, never refused — a launch must not fail over a
        // rate — and 0 means "the default rate".
        int hz = 165000;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, 165000);
        // The request is guarded against what the DRIVER takes: the bench's
        // MW_VDD_REFRESH reaches past the product's ceiling.
        hz = 500000;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, 500000);
        hz = 1000000;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, kRateDriverMax * kMilliHz);
        hz = 1000;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, kRateMin * kMilliHz);
        hz = 0;
        CHECK(!normaliseRate(hz));
        CHECK_EQ(hz, 0);
        hz = -60000;
        CHECK(!normaliseRate(hz));
        CHECK_EQ(hz, 0);

        // It reaches the elevated helper with the request, in millihertz,
        // junk and absence both meaning "the default rate". An older server's
        // whole hertz is read too.
        const auto req = parseRequest("{\"action\":\"activate\",\"refresh_mhz\":239744}", &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(req->refreshMilliHz, 239744);
        const auto old = parseRequest("{\"action\":\"activate\",\"refresh\":165}", &err);
        CHECK(old.has_value());
        if (old) CHECK_EQ(old->refreshMilliHz, 165000);
        const auto none = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(none.has_value());
        if (none) CHECK_EQ(none->refreshMilliHz, 0);
        const auto bad = parseRequest("{\"action\":\"activate\",\"refresh_mhz\":\"fast\"}", &err);
        CHECK(bad.has_value());
        if (bad) CHECK_EQ(bad->refreshMilliHz, 0);
        Request r;
        r.action = Request::Action::Activate;
        r.refreshMilliHz = 59940;
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) CHECK_EQ(back->refreshMilliHz, 59940);
        CHECK(!toJson(Request{}).contains("refresh"));

        // Every mode in the file we write carries it — the display exists to
        // show frames at the client's cadence, whatever the size — and the
        // driver's global list leads with it.
        const QString xml = settingsXml(0, 0, 60000);
        CHECK_EQ(xml.count(QStringLiteral("<refresh_rate>120</refresh_rate>")), 0);
        CHECK(xml.count(QStringLiteral("<refresh_rate>60</refresh_rate>")) >= 4);
        CHECK(xml.indexOf(QStringLiteral("<g_refresh_rate>60</g_refresh_rate>")) <
              xml.indexOf(QStringLiteral("<g_refresh_rate>144</g_refresh_rate>")));
        // No rate measured: the default stands, and the file is the one the
        // installer has always written.
        CHECK_EQ(settingsXml(0, 0, 0), settingsXml());
    }

    SECTION("VirtualDisplay — a stream larger than the default mode gets a display of its size");
    {
        // "Match my screen": the client's size, smaller than the default too.
        CHECK_EQ(sizeForStream(1280, 720, true).width, 1280);
        CHECK_EQ(sizeForStream(1280, 720, true).height, 720);
        // Larger than 1920 x 1080: its own size, without "Match my screen"
        // (07/11: a TV's 1440p stream left at 1080p).
        CHECK_EQ(sizeForStream(2560, 1440, false).width, 2560);
        CHECK_EQ(sizeForStream(2560, 1440, false).height, 1440);
        CHECK_EQ(sizeForStream(3840, 2160, false).width, 3840);
        CHECK_EQ(sizeForStream(3440, 1440, false).width, 3440);
        // 1920 x 1200 holds more pixels than 1080p: its own size too.
        CHECK_EQ(sizeForStream(1920, 1200, false).height, 1200);
        // The default size, or smaller: the display's own mode, as before.
        CHECK_EQ(sizeForStream(1920, 1080, false).width, 0);
        CHECK_EQ(sizeForStream(1280, 720, false).width, 0);
        CHECK_EQ(sizeForStream(1280, 720, false).height, 0);
        // No size asked (height 0, native): nothing either way.
        CHECK_EQ(sizeForStream(0, 0, true).width, 0);
        CHECK_EQ(sizeForStream(0, 0, false).width, 0);
    }

    SECTION("VirtualDisplay — the stream's rate under the product's ceiling, the bench's past it");
    {
        // The product: the stream's own rate, never past kRateMax, whatever
        // a page sent as its stream_fps.
        CHECK_EQ(refreshForStream(120, 0), 120);
        CHECK_EQ(refreshForStream(400, 0), kRateMax);
        CHECK_EQ(refreshForStream(10, 0), kRateMin);
        CHECK_EQ(refreshForStream(0, 0), 0);
        // MW_VDD_REFRESH: its own rate, up to the driver's ceiling, whatever
        // the stream's.
        CHECK_EQ(refreshForStream(60, 500), 500);
        CHECK_EQ(refreshForStream(60, 240), 240);
        CHECK_EQ(refreshForStream(0, 165), 165);
        CHECK_EQ(refreshForStream(60, 900), kRateDriverMax);
        // The product on Windows (decision A): kRateMax whatever the stream
        // asks, measured or not; the bench still wins.
        CHECK_EQ(refreshForStream(60, 0, true), kRateMax);
        CHECK_EQ(refreshForStream(144, 0, true), kRateMax);
        CHECK_EQ(refreshForStream(0, 0, true), kRateMax);
        CHECK_EQ(refreshForStream(60, 500, true), 500);
        // 500 travels to the elevated helper and into the driver's list.
        const auto req = parseRequest("{\"action\":\"activate\",\"refresh_mhz\":500000}", &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(req->refreshMilliHz, 500000);
        CHECK(settingsXml(0, 0, 500000)
                  .contains(QStringLiteral("<g_refresh_rate>500</g_refresh_rate>")));
    }

    SECTION("VirtualDisplay — a rate to the millihertz, as the driver and Windows spell it");
    {
        // The driver's file: whole hertz as they always were, decimals as
        // short as they go.
        CHECK_EQ(rateText(240000), QStringLiteral("240"));
        CHECK_EQ(rateText(239744), QStringLiteral("239.744"));
        CHECK_EQ(rateText(59940), QStringLiteral("59.94"));
        CHECK_EQ(rateText(119500), QStringLiteral("119.5"));
        CHECK_EQ(rateText(60001), QStringLiteral("60.001"));
        // Windows' mode: the fraction in lowest terms. 239.744 Hz is the
        // 29968/125 V0 saw applied on DualRTX (14/10/2026).
        CHECK_EQ(rateFraction(239744).numerator, 29968u);
        CHECK_EQ(rateFraction(239744).denominator, 125u);
        CHECK_EQ(rateFraction(240000).numerator, 240u);
        CHECK_EQ(rateFraction(240000).denominator, 1u);
        CHECK_EQ(rateFraction(59940).numerator, 2997u);
        CHECK_EQ(rateFraction(59940).denominator, 50u);
        CHECK_EQ(rateFraction(0).numerator, 0u);
        // Into the file we write, and into an owner's list: the same spelling,
        // and the mode comes out again byte for byte.
        CHECK(settingsXml(0, 0, 239744)
                  .contains(QStringLiteral("<refresh_rate>239.744</refresh_rate>")));
        CHECK(settingsXml(0, 0, 239744)
                  .contains(QStringLiteral("<g_refresh_rate>239.744</g_refresh_rate>")));
        const QString theirs =
            QStringLiteral("<vdd_settings>\n  <resolutions>\n"
                           "    <resolution>\n      <width>1920</width>\n"
                           "      <height>1080</height>\n      <refresh_rate>240</refresh_rate>\n"
                           "    </resolution>\n  </resolutions>\n</vdd_settings>\n");
        bool changed = false;
        const QString grown = settingsWithMode(theirs, 1920, 1080, 239744, &changed);
        CHECK(changed);
        CHECK(grown.indexOf(QStringLiteral("<refresh_rate>239.744</refresh_rate>")) <
              grown.indexOf(QStringLiteral("<refresh_rate>240</refresh_rate>")));
        // 240 is listed already: nothing to add.
        settingsWithMode(theirs, 1920, 1080, 240000, &changed);
        CHECK(!changed);
        CHECK_EQ(settingsWithoutMode(grown, 1920, 1080, 239744, &changed), theirs);
        CHECK(changed);
        // The record says it the same way, and reads back whole hertz from
        // before millihertz.
        const QList<AddedMode> modes = parseAddedModes(
            QStringLiteral("1920x1080@239.744\n1784x1160@240\n1280x720@59.94\n1x1@2.5555\n"));
        CHECK_EQ(int(modes.size()), 3);
        CHECK(modes.value(0) == (AddedMode{1920, 1080, 239744}));
        CHECK(modes.value(1) == (AddedMode{1784, 1160, 240000}));
        CHECK(modes.value(2) == (AddedMode{1280, 720, 59940}));
        CHECK_EQ(addedModesText(modes),
                 QStringLiteral("1920x1080@239.744\n1784x1160@240\n1280x720@59.94\n"));
    }

    SECTION("VirtualDisplay — matched to the client's screen: k times its rate, or nothing");
    {
        // The UM790Pro's M27Q "120 Hz" runs at 119.872 (plan glissade, P2):
        // twice it, where 240.000 loses a picture every 7.8 s.
        CHECK_EQ(matchedRefreshMilliHz(119872, 120, kMatchedRateMaxHz), 239744);
        // The same screen with a 60 fps stream: a frame spans two refreshes,
        // and the display at twice the screen keeps both on its grid.
        CHECK_EQ(matchedRefreshMilliHz(119872, 60, kMatchedRateMaxHz), 239744);
        // 59.94 and 60 Hz: four times — five would be 299.7, but a frame of
        // five presents leaves "Auto"'s step to twice the rate between two.
        CHECK_EQ(matchedRefreshMilliHz(59940, 60, kMatchedRateMaxHz), 239760);
        CHECK_EQ(matchedRefreshMilliHz(60001, 60, kMatchedRateMaxHz), 240004);
        // Bruno's rule (14/10/2026): 288 for 144, 330 for 165.
        CHECK_EQ(matchedRefreshMilliHz(144000, 144, kMatchedRateMaxHz), 288000);
        CHECK_EQ(matchedRefreshMilliHz(165000, 165, kMatchedRateMaxHz), 330000);
        CHECK_EQ(matchedRefreshMilliHz(164800, 165, kMatchedRateMaxHz), 329600);
        // A 240 Hz screen is matched once (×1); a 100 Hz one twice, not
        // three times (an odd three presents a frame).
        CHECK_EQ(matchedRefreshMilliHz(239960, 240, kMatchedRateMaxHz), 239960);
        CHECK_EQ(matchedRefreshMilliHz(100000, 100, kMatchedRateMaxHz), 200000);
        // A lower ceiling (the bench's vddmatchmax): what fits under it.
        CHECK_EQ(matchedRefreshMilliHz(144000, 144, 240), 144000);
        CHECK_EQ(matchedRefreshMilliHz(144000, 144, 500), 288000);
        CHECK_EQ(matchedRefreshMilliHz(60000, 60, 500), 480000);
        // Nothing fits: no measurement, a stream that is no divisor of the
        // screen (Auto's 120 for a 144 Hz panel), a stream faster than the
        // screen, a screen faster than the ceiling.
        CHECK_EQ(matchedRefreshMilliHz(0, 120, kMatchedRateMaxHz), 0);
        CHECK_EQ(matchedRefreshMilliHz(144000, 120, kMatchedRateMaxHz), 0);
        CHECK_EQ(matchedRefreshMilliHz(60000, 120, kMatchedRateMaxHz), 0);
        CHECK_EQ(matchedRefreshMilliHz(360000, 360, kMatchedRateMaxHz), 0);
        CHECK_EQ(matchedRefreshMilliHz(119872, 0, kMatchedRateMaxHz), 0);
        // The gate's tolerance, 0.2 %: 119.872 against 120 is in (0.107 %),
        // 119.5 is not (0.42 %).
        CHECK_EQ(matchedRefreshMilliHz(119500, 120, kMatchedRateMaxHz), 0);
        // The product's: on under Windows since V2 and the eye test, off
        // elsewhere.
#if defined(Q_OS_WIN)
        CHECK(kVddMatchProduct);
#else
        CHECK(!kVddMatchProduct);
#endif
    }

    SECTION("VirtualDisplay — under the rates the capture did not hold");
    {
        // Refused an hour before `now`, the first time.
        const qint64 now = 1'800'000'000'000LL;
        const qint64 hour = 3'600'000LL;
        const auto refusal = [now, hour](const char* gpu, int milliHz, const char* version) {
            return QJsonObject{{QStringLiteral("gpu"), QString::fromLatin1(gpu)},
                               {QStringLiteral("milli_hz"), milliHz},
                               {QStringLiteral("version"), QString::fromLatin1(version)},
                               {QStringLiteral("at"), now - hour},
                               {QStringLiteral("count"), 1}};
        };
        const QStringList gpus{QStringLiteral("GPU A"), QStringLiteral("GPU B")};
        // Nothing refused: the product's ceiling.
        CHECK_EQ(matchCapAfterRefusals(kMatchedRateMaxHz, {}, "1.0", gpus, now), kMatchedRateMaxHz);
        // 330 refused: a 165 Hz screen gets its own rate (×1), a 144 Hz one
        // still tries 288.
        QJsonArray refused{refusal("GPU A", 330000, "1.0")};
        int cap = matchCapAfterRefusals(kMatchedRateMaxHz, refused, "1.0", gpus, now);
        CHECK_EQ(cap, 323);
        CHECK_EQ(matchedRefreshMilliHz(165000, 165, cap), 165000);
        CHECK_EQ(matchedRefreshMilliHz(144000, 144, cap), 288000);
        CHECK_EQ(matchedRefreshMilliHz(119872, 120, cap), 239744);
        // 288 refused too: the lowest one rules, and 144 gets 144.
        refused.append(refusal("GPU A", 287694, "1.0"));
        cap = matchCapAfterRefusals(kMatchedRateMaxHz, refused, "1.0", gpus, now);
        CHECK_EQ(cap, 281);
        CHECK_EQ(matchedRefreshMilliHz(143847, 144, cap), 143847);
        CHECK_EQ(matchedRefreshMilliHz(119872, 120, cap), 239744);
        // Another version's refusals, a GPU this machine no longer has: none.
        CHECK_EQ(matchCapAfterRefusals(kMatchedRateMaxHz, refused, "1.1", gpus, now),
                 kMatchedRateMaxHz);
        CHECK_EQ(matchCapAfterRefusals(kMatchedRateMaxHz, refused, "1.0", {"GPU C"}, now),
                 kMatchedRateMaxHz);
        // No GPU known yet: every refusal of this version counts.
        CHECK_EQ(matchCapAfterRefusals(kMatchedRateMaxHz, refused, "1.0", {}, now), 281);
        // Never above the ceiling it was given (the bench's vddmatchmax).
        CHECK_EQ(matchCapAfterRefusals(240, refused, "1.0", gpus, now), 240);
        // A malformed entry is skipped.
        CHECK_EQ(
            matchCapAfterRefusals(kMatchedRateMaxHz, QJsonArray{QJsonObject{}}, "1.0", gpus, now),
            kMatchedRateMaxHz);
    }

    SECTION("VirtualDisplay — a refusal holds the cap for a while, not for the version (D27)");
    {
        constexpr qint64 hour = 3'600'000LL;
        // 12 h, doubled for each refusal in a row, at most 14 days.
        CHECK_EQ(refusalHoldMs(1), 12 * hour);
        CHECK_EQ(refusalHoldMs(2), 24 * hour);
        CHECK_EQ(refusalHoldMs(3), 48 * hour);
        CHECK_EQ(refusalHoldMs(5), 192 * hour);
        CHECK_EQ(refusalHoldMs(6), 14 * 24 * hour);
        CHECK_EQ(refusalHoldMs(16), 14 * 24 * hour);
        CHECK_EQ(refusalHoldMs(0), 12 * hour);

        const qint64 now = 1'800'000'000'000LL;
        const QStringList gpus{QStringLiteral("GPU A")};
        const auto refused = [](qint64 at, int count) {
            QJsonObject o{{QStringLiteral("gpu"), QStringLiteral("GPU A")},
                          {QStringLiteral("milli_hz"), 330000},
                          {QStringLiteral("version"), QStringLiteral("1.0")},
                          {QStringLiteral("count"), count}};
            if (at > 0) o.insert(QStringLiteral("at"), at);
            return QJsonArray{o};
        };
        // In force for its hold, and the next stream after it tries again.
        CHECK_EQ(
            matchCapAfterRefusals(kMatchedRateMaxHz, refused(now - 11 * hour, 1), "1.0", gpus, now),
            323);
        CHECK_EQ(
            matchCapAfterRefusals(kMatchedRateMaxHz, refused(now - 12 * hour, 1), "1.0", gpus, now),
            kMatchedRateMaxHz);
        // The third in a row holds two days.
        CHECK_EQ(
            matchCapAfterRefusals(kMatchedRateMaxHz, refused(now - 47 * hour, 3), "1.0", gpus, now),
            323);
        CHECK_EQ(
            matchCapAfterRefusals(kMatchedRateMaxHz, refused(now - 48 * hour, 3), "1.0", gpus, now),
            kMatchedRateMaxHz);
        // Without a date — kept by a build before the hold expired, like the
        // bench's of 28/10 at 02:17 — or dated ahead of the clock: tried again.
        CHECK_EQ(matchCapAfterRefusals(kMatchedRateMaxHz, refused(0, 1), "1.0", gpus, now),
                 kMatchedRateMaxHz);
        CHECK_EQ(matchCapAfterRefusals(kMatchedRateMaxHz, refused(now + hour, 1), "1.0", gpus, now),
                 kMatchedRateMaxHz);
    }

    SECTION("VirtualDisplay — ours goes on the right of the others' layout, edge to edge");
    {
        // The N95 of 16/10/2026: Display 1 primary, the owner's VDD on its
        // right and higher up. Ours goes on the VDD's right, top edges aligned.
        const QList<DesktopRect> n95{{0, 0, 1920, 1080}, {1920, -360, 2560, 1440}};
        CHECK_EQ(placeBeside(n95), QPoint(4480, -360));
        // Listed the other way round: the same place.
        CHECK_EQ(placeBeside({n95[1], n95[0]}), QPoint(4480, -360));
        // A display on the LEFT of the primary (negative x): the rightmost
        // still wins.
        CHECK_EQ(placeBeside({{-2560, 0, 2560, 1440}, {0, 0, 1920, 1080}}), QPoint(1920, 0));
        // Two right edges level (a stack): beside the topmost.
        CHECK_EQ(placeBeside({{0, 1080, 1920, 1080}, {0, 0, 1920, 1080}}), QPoint(1920, 0));
        // Nothing else on, or nothing usable: the origin.
        CHECK_EQ(placeBeside({}), QPoint(0, 0));
        CHECK_EQ(placeBeside({{500, 500, 0, 0}}), QPoint(0, 0));
    }

    SECTION("VirtualDisplay — the bundled driver is pinned file by file");
    CHECK_EQ(int(driverFiles().size()), 3);
    bool inf = false, cat = false, dll = false;
    for (const DriverFile& f : driverFiles()) {
        CHECK(isHex(QString::fromLatin1(f.sha256), 64));
        const QString name = QString::fromLatin1(f.name);
        CHECK(!name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')));
        inf = inf || name.endsWith(QLatin1String(".inf"), Qt::CaseInsensitive);
        cat = cat || name.endsWith(QLatin1String(".cat"), Qt::CaseInsensitive);
        dll = dll || name.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive);
    }
    CHECK(inf && cat && dll);
    CHECK(isHex(catalogSignerThumbprint(), 40));
    CHECK_EQ(hardwareId(), QStringLiteral("Root\\MttVDD"));

    SECTION("VirtualDisplay — exclusive is a boolean of Activate, written only when asked");
    {
        auto req = parseRequest("{\"action\":\"activate\",\"exclusive\":true}", &err);
        CHECK(req.has_value() && req->exclusive);
        req = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(req.has_value() && !req->exclusive);
        CHECK(!parseRequest("{\"action\":\"activate\",\"exclusive\":\"yes\"}", &err).has_value());
        CHECK(!parseRequest("{\"action\":\"activate\",\"exclusive\":1}", &err).has_value());
        // Another verb never carries it into the helper.
        req = parseRequest("{\"action\":\"deactivate\",\"exclusive\":true}", &err);
        CHECK(req.has_value() && !req->exclusive);
        Request r;
        r.exclusive = true;
        req = parseRequest(toJson(r), &err);
        CHECK(req.has_value() && req->exclusive);
        r.exclusive = false;
        CHECK(!toJson(r).contains("exclusive"));
    }

    SECTION("VirtualDisplay — bench_hdr is a boolean of Activate, written only when asked");
    {
        auto req = parseRequest("{\"action\":\"activate\",\"bench_hdr\":true}", &err);
        CHECK(req.has_value() && req->benchHdr);
        req = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(req.has_value() && !req->benchHdr);
        CHECK(!parseRequest("{\"action\":\"activate\",\"bench_hdr\":1}", &err).has_value());
        // Another verb never carries it into the helper.
        req = parseRequest("{\"action\":\"deactivate\",\"bench_hdr\":true}", &err);
        CHECK(req.has_value() && !req->benchHdr);
        Request r;
        r.benchHdr = true;
        req = parseRequest(toJson(r), &err);
        CHECK(req.has_value() && req->benchHdr);
        r.benchHdr = false;
        CHECK(!toJson(r).contains("bench_hdr"));
        // The macOS flag alone is never written: Windows stays SDR without the bench.
        r.hdr = true;
        CHECK(!toJson(r).contains("hdr"));
    }

    SECTION("VirtualDisplay — bench_secondary is a boolean of Activate, written only when asked");
    {
        auto req = parseRequest("{\"action\":\"activate\",\"bench_secondary\":true}", &err);
        CHECK(req.has_value() && req->benchSecondary);
        req = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(req.has_value() && !req->benchSecondary);
        CHECK(!parseRequest("{\"action\":\"activate\",\"bench_secondary\":\"yes\"}", &err)
                   .has_value());
        // Another verb never carries it into the helper.
        req = parseRequest("{\"action\":\"deactivate\",\"bench_secondary\":true}", &err);
        CHECK(req.has_value() && !req->benchSecondary);
        Request r;
        r.benchSecondary = true;
        req = parseRequest(toJson(r), &err);
        CHECK(req.has_value() && req->benchSecondary && !req->benchHdr);
        r.benchSecondary = false;
        CHECK(!toJson(r).contains("bench_secondary"));
    }

    SECTION("VirtualDisplay — the GPU the owner chose travels as a name, and only as text");
    {
        // A GPU as Windows names it, into the helper and back.
        auto req =
            parseRequest("{\"action\":\"activate\",\"gpu\":\"NVIDIA GeForce RTX 5060 Ti\"}", &err);
        CHECK(req.has_value() && req->gpu == QStringLiteral("NVIDIA GeForce RTX 5060 Ti"));
        Request r;
        r.gpu = QStringLiteral("Intel(R) Arc(TM) A380 Graphics");
        req = parseRequest(toJson(r), &err);
        CHECK(req.has_value() && req->gpu == r.gpu);
        CHECK(!toJson(Request{}).contains("gpu"));
        // Nothing the driver's XML could read as markup, nothing that is not
        // a string: the whole request is refused, not half kept.
        CHECK(!parseRequest("{\"action\":\"activate\",\"gpu\":\"</friendlyname><x>\"}", &err)
                   .has_value());
        CHECK(!parseRequest("{\"action\":\"activate\",\"gpu\":\"A & B\"}", &err).has_value());
        CHECK(!parseRequest("{\"action\":\"activate\",\"gpu\":7}", &err).has_value());
        CHECK(!parseRequest("{\"action\":\"activate\",\"gpu\":\" \"}", &err).has_value());
        // Another verb never carries it into the helper.
        req = parseRequest("{\"action\":\"deactivate\",\"gpu\":\"AMD Radeon(TM) Graphics\"}", &err);
        CHECK(req.has_value() && req->gpu.isEmpty());
        CHECK(validGpuName(QStringLiteral("AMD Radeon(TM) Graphics")));
        CHECK(!validGpuName(QString()));
        CHECK(!validGpuName(QString(129, QLatin1Char('x'))));
        CHECK(!validGpuName(QStringLiteral("RTX\n5060")));

        // Into the file we write — unless a bench names its own GPU — and
        // read back, ours or an owner's; "default" is no GPU at all.
        if (qEnvironmentVariableIsEmpty("MW_VDD_GPU")) {
            const QString chosen = settingsXml(0, 0, 0, QStringLiteral("AMD Radeon(TM) Graphics"));
            CHECK(chosen.contains(
                QStringLiteral("<friendlyname>AMD Radeon(TM) Graphics</friendlyname>")));
            CHECK_EQ(gpuOfSettings(chosen), QStringLiteral("AMD Radeon(TM) Graphics"));
            CHECK(isOurSettings(chosen));
            CHECK(settingsXml().contains(QStringLiteral("<friendlyname>default</friendlyname>")));
            CHECK(gpuOfSettings(settingsXml()).isEmpty());
            // A name that could not travel never reaches the file either.
            CHECK(settingsXml(0, 0, 0, QStringLiteral("<x>"))
                      .contains(QStringLiteral("<friendlyname>default</friendlyname>")));
        }
        const QString theirs = QStringLiteral("<vdd_settings>\n  <gpu>\n    <friendlyname>"
                                              "NVIDIA GeForce RTX 5060 Ti</friendlyname>\n"
                                              "  </gpu>\n</vdd_settings>\n");
        CHECK_EQ(gpuOfSettings(theirs), QStringLiteral("NVIDIA GeForce RTX 5060 Ti"));
        CHECK(gpuOfSettings(QStringLiteral("<vdd_settings/>")).isEmpty());
    }

    SECTION("VirtualDisplay — a layout with the other displays off is never saved");
    {
        const LayoutStep all[] = {LayoutStep::Impose,  LayoutStep::Prune,     LayoutStep::Mode,
                                  LayoutStep::Primary, LayoutStep::Exclusive, LayoutStep::Restore};
        // Off: only Impose saves, and it turns them back on in the same change.
        for (const LayoutStep s : all) {
            if (s == LayoutStep::Impose) continue;
            CHECK(!savesLayout(s, true));
        }
        CHECK(savesLayout(LayoutStep::Impose, true));
        CHECK(!othersOffAfter(LayoutStep::Impose, true, true));
        // The exclusive step and the way back never save, the others on or off.
        CHECK(!savesLayout(LayoutStep::Exclusive, false));
        CHECK(!savesLayout(LayoutStep::Restore, false));
        // On: what the helper always did — the layout of a stream is saved.
        CHECK(savesLayout(LayoutStep::Mode, false) && savesLayout(LayoutStep::Primary, false) &&
              savesLayout(LayoutStep::Prune, false));

        // Every sequence of up to five steps, each going through or not: once
        // the exclusive step has gone through, nothing saves until a step
        // that turns the others back on has.
        const int n = int(std::size(all));
        for (int len = 1; len <= 5; ++len) {
            int combos = 1;
            for (int i = 0; i < len; ++i)
                combos *= n * 2;
            for (int c = 0; c < combos; ++c) {
                bool off = false;
                int code = c;
                for (int i = 0; i < len; ++i) {
                    const LayoutStep s = all[code % n];
                    code /= n;
                    const bool ok = code % 2;
                    code /= 2;
                    if (off && savesLayout(s, off)) {
                        // The only save allowed while off brings them back on.
                        CHECK(s == LayoutStep::Impose);
                        if (ok) CHECK(!othersOffAfter(s, off, ok));
                    }
                    off = othersOffAfter(s, off, ok);
                }
            }
        }
        // A failed step changes nothing: off stays off, on stays on.
        CHECK(othersOffAfter(LayoutStep::Exclusive, false, true));
        CHECK(!othersOffAfter(LayoutStep::Exclusive, false, false));
        CHECK(othersOffAfter(LayoutStep::Restore, true, false));
        CHECK(othersOffAfter(LayoutStep::Mode, true, true));
    }

    SECTION("VirtualDisplay — only the owner's stream turns the other displays off");
    // Alone on the display, the option decides.
    CHECK(othersGoOff(Asker::Owner, true, false, false));
    CHECK(!othersGoOff(Asker::Owner, false, false, false));
    // A guest on it first: they stay on for this stream.
    CHECK(!othersGoOff(Asker::Owner, true, true, false));
    // Off already, a new mode of the owner's keeps them off beside a guest.
    CHECK(othersGoOff(Asker::Owner, true, true, true));
    // A guest never asks for it, whatever the rest says.
    for (const bool option : {false, true})
        for (const bool guest : {false, true})
            for (const bool off : {false, true})
                CHECK(!othersGoOff(Asker::Guest, option, guest, off));

    SECTION("VirtualDisplay — the admin allows, the viewer chooses");
    // Both keys, when the browser chose.
    CHECK(ownerWantsOthersOff(true, true, true, false));
    CHECK(!ownerWantsOthersOff(true, true, false, true));
    CHECK(!ownerWantsOthersOff(false, true, true, true));
    // A browser that never chose: what the PC did before (ticked box on update).
    CHECK(ownerWantsOthersOff(true, false, false, true));
    CHECK(!ownerWantsOthersOff(true, false, true, false));
    // Never without the admin's leave, whatever the browser holds.
    for (const bool has : {false, true})
        for (const bool choice : {false, true})
            for (const bool inherited : {false, true})
                CHECK(!ownerWantsOthersOff(false, has, choice, inherited));

    SECTION("VirtualDisplay — the cards of the displays a stream turned off");
    CHECK(darkDisplays({1, 2, 3}, {1}, true) == QList<int>({2, 3}));
    CHECK(darkDisplays({2, 3}, {7}, true) == QList<int>({2, 3}));
    CHECK(darkDisplays({2, 3}, {2, 3}, true).isEmpty());
    CHECK(darkDisplays({2, 3}, {7}, false).isEmpty());
    CHECK(darkDisplays({}, {7}, true).isEmpty());
    CHECK(darkDisplays({2, 2}, {}, true) == QList<int>({2}));

    SECTION("VirtualDisplay — the other displays come back with the owner gone");
    CHECK(othersComeBack(false, true));
    CHECK(!othersComeBack(true, true));
    CHECK(!othersComeBack(false, false));
    CHECK(!othersComeBack(true, false));
    {
        // The verb reaches the helper as itself, and asks for no exclusive.
        Request back;
        back.action = Request::Action::OthersBack;
        back.exclusive = true;
        const auto parsed = parseRequest(toJson(back), nullptr);
        CHECK(parsed.has_value());
        if (parsed) {
            CHECK(parsed->action == Request::Action::OthersBack);
            CHECK(!parsed->exclusive);
        }
    }

    SECTION("VirtualDisplay — the guard restores only a dead server's displays left off");
    // Off and the server running: it waits. The server gone with them off:
    // it brings them back. Not off — or a marker it cannot read — whatever
    // the server does: it leaves, touching nothing.
    CHECK(guardStep(true, true) == GuardStep::Wait);
    CHECK(guardStep(false, true) == GuardStep::Restore);
    CHECK(guardStep(true, false) == GuardStep::Leave);
    CHECK(guardStep(false, false) == GuardStep::Leave);
    CHECK_EQ(guardArgument(), QStringLiteral("--vdisplay-guard"));
}
