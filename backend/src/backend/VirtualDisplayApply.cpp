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

#include "backend/VirtualDisplayApply.h"

#include "backend/DriverPackage.h"
#include "backend/VirtualDisplay.h"
#include "common/Logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QThread>

#include <cstdio>
#include <optional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <wincrypt.h>
#include <vector>
#endif

// The helper behind `--vdisplay-apply`: five verbs on "MoonlightWeb Virtual
// Display" (VirtualDisplay.h), read from a request file, answered in a result
// file and on stdout.
//
//   install     installer, elevated — create our device node from the bundled
//               driver, named, and leave it DISABLED. No-op when it exists.
//   uninstall   uninstaller, elevated — remove our node; the driver package
//               and its settings only if no other node uses them.
//   activate    a stream starts — enable the node ("driver" stage, elevated),
//               then on the desktop ("mode" stage): our mode, SDR, and make
//               it the primary display, remembering which one was.
//   deactivate  the last stream ended — give the primary role back ("mode"),
//               then disable the node ("driver").
//
// The two stages exist because a service runs the elevated half as SYSTEM in
// session 0, where there is no desktop to set a mode on, and the desktop half
// in the console session as the user. The installer's task runs both at once
// (`--stage=all`), in the order the verb needs.

namespace VirtualDisplayApply {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitReboot = 3010; // ERROR_SUCCESS_REBOOT_REQUIRED, like a driver installer

struct Context
{
    QString dir;
    VirtualDisplay::Request request;
    VirtualDisplay::Result result;
};

/// The verdict goes to the result file (the task path has no stdout to read)
/// AND to stdout (the child paths read that). Both, always: the two readers
/// never know which way they were reached.
int finish(Context& ctx)
{
    const QByteArray json = VirtualDisplay::toJson(ctx.result);
    QFile f(ctx.dir + QStringLiteral("/result.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(json);
        f.close();
    } else {
        Logger::warning(QStringLiteral("[vdisplay-apply] cannot write %1").arg(f.fileName()));
    }
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    std::fflush(stdout);
    if (ctx.result.ok)
        Logger::info(QStringLiteral("[vdisplay-apply] ok (%1)").arg(ctx.result.stage));
    else
        Logger::warning(QStringLiteral("[vdisplay-apply] failed at %1: %2")
                            .arg(ctx.result.stage, ctx.result.error));
    if (!ctx.result.ok) return kExitFailed;
    return ctx.result.rebootRequired ? kExitReboot : kExitOk;
}

int failAt(Context& ctx, const QString& stage, const QString& error)
{
    ctx.result.ok = false;
    ctx.result.stage = stage;
    ctx.result.error = error;
    return finish(ctx);
}

#ifdef Q_OS_WIN

QString lastErrorText(DWORD err = GetLastError())
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QString();
    if (buf) LocalFree(buf);
    return QStringLiteral("0x%1 %2").arg(err, 8, 16, QLatin1Char('0')).arg(text);
}

// The pinned hashes and the catalog signer's trust: DriverPackage.h, shared
// with "MoonlightWeb Virtual HID".

// ── Device nodes ────────────────────────────────────────────────────────────

/// A device info set holding every Display-class node, present or not, with
/// a way to find ours (hardware id + friendly name) among them.
struct DisplayNodes
{
    HDEVINFO set = INVALID_HANDLE_VALUE;

    DisplayNodes()
        : set(SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0))
    {}
    ~DisplayNodes()
    {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
    bool valid() const { return set != INVALID_HANDLE_VALUE; }

    QString property(const SP_DEVINFO_DATA& data, DWORD prop) const
    {
        DWORD size = 0;
        SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data), prop, nullptr,
                                          nullptr, 0, &size);
        if (size == 0) return QString();
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data), prop,
                                               nullptr, reinterpret_cast<BYTE*>(buf.data()), size,
                                               nullptr))
            return QString();
        return QString::fromWCharArray(buf.data()); // the first string of a MULTI_SZ
    }

    bool hasHardwareId(const SP_DEVINFO_DATA& data, const QString& wanted) const
    {
        DWORD size = 0;
        SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data),
                                          SPDRP_HARDWAREID, nullptr, nullptr, 0, &size);
        if (size == 0) return false;
        std::vector<wchar_t> ids(size / sizeof(wchar_t) + 1, 0);
        if (!SetupDiGetDeviceRegistryPropertyW(set, const_cast<SP_DEVINFO_DATA*>(&data),
                                               SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<BYTE*>(ids.data()), size, nullptr))
            return false;
        for (const wchar_t* h = ids.data(); *h; h += wcslen(h) + 1)
            if (QString::fromWCharArray(h).compare(wanted, Qt::CaseInsensitive) == 0) return true;
        return false;
    }

    /// Every node with the driver's hardware id; `ours` set on the one that
    /// carries our friendly name.
    struct Node
    {
        SP_DEVINFO_DATA data{};
        bool ours = false;
    };
    std::vector<Node> driverNodes() const
    {
        std::vector<Node> out;
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
            if (!hasHardwareId(data, VirtualDisplay::hardwareId())) continue;
            Node n;
            n.data = data;
            n.ours = property(data, SPDRP_FRIENDLYNAME)
                         .compare(VirtualDisplay::displayName(), Qt::CaseInsensitive) == 0;
            out.push_back(n);
        }
        return out;
    }

    std::optional<SP_DEVINFO_DATA> ours() const
    {
        for (const Node& n : driverNodes())
            if (n.ours) return n.data;
        return std::nullopt;
    }
};

bool setEnabled(HDEVINFO set, SP_DEVINFO_DATA& data, bool enable, QString* error)
{
    // What Device Manager's Enable/Disable does: a property change, global
    // then for the current hardware profile (the second is what makes it
    // take effect at once; devcon does the same pair).
    for (const DWORD scope : {DWORD(DICS_FLAG_GLOBAL), DWORD(DICS_FLAG_CONFIGSPECIFIC)}) {
        SP_PROPCHANGE_PARAMS params{};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
        params.Scope = scope;
        params.HwProfile = 0;
        if (!SetupDiSetClassInstallParamsW(set, &data, &params.ClassInstallHeader,
                                           sizeof(params)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &data)) {
            if (scope == DICS_FLAG_GLOBAL) {
                *error = QStringLiteral("%1 the device: %2")
                             .arg(enable ? QStringLiteral("enabling") : QStringLiteral("disabling"))
                             .arg(lastErrorText());
                return false;
            }
        }
    }
    Logger::info(QStringLiteral("[vdisplay-apply] device node %1")
                     .arg(enable ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    return true;
}

bool nodeIsEnabled(SP_DEVINFO_DATA& data)
{
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) != CR_SUCCESS) return false;
    return !((status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED);
}

/// The name that makes a node ours — in Device Manager and to every lookup.
bool setFriendlyName(HDEVINFO set, SP_DEVINFO_DATA& data, QString* error)
{
    const std::wstring name = VirtualDisplay::displayName().toStdWString();
    if (!SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_FRIENDLYNAME,
                                           reinterpret_cast<const BYTE*>(name.c_str()),
                                           DWORD((name.size() + 1) * sizeof(wchar_t)))) {
        *error = QStringLiteral("SetupDiSetDeviceRegistryProperty(friendly name): %1")
                     .arg(lastErrorText());
        return false;
    }
    return true;
}

/// Create our node: a root-enumerated Display-class device with the driver's
/// hardware id and our friendly name, registered but not yet driven.
bool createOurNode(HDEVINFO set, SP_DEVINFO_DATA& data, QString* error)
{
    data = SP_DEVINFO_DATA{};
    data.cbSize = sizeof(data);
    if (!SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                  DICD_GENERATE_ID, &data)) {
        *error = QStringLiteral("SetupDiCreateDeviceInfo: %1").arg(lastErrorText());
        return false;
    }
    // A REG_MULTI_SZ: the id, then two terminators.
    std::wstring hw = VirtualDisplay::hardwareId().toStdWString();
    std::vector<wchar_t> multi(hw.begin(), hw.end());
    multi.push_back(0);
    multi.push_back(0);
    if (!SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(multi.data()),
                                           DWORD(multi.size() * sizeof(wchar_t)))) {
        *error = QStringLiteral("SetupDiSetDeviceRegistryProperty(hardware id): %1")
                     .arg(lastErrorText());
        return false;
    }
    if (!setFriendlyName(set, data, error)) return false;
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &data)) {
        *error = QStringLiteral("DIF_REGISTERDEVICE: %1").arg(lastErrorText());
        return false;
    }
    return true;
}

bool removeNode(HDEVINFO set, SP_DEVINFO_DATA& data, QString* error)
{
    SP_REMOVEDEVICE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    params.Scope = DI_REMOVEDEVICE_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &data, &params.ClassInstallHeader, sizeof(params)) ||
        !SetupDiCallClassInstaller(DIF_REMOVE, set, &data)) {
        *error = QStringLiteral("DIF_REMOVE: %1").arg(lastErrorText());
        return false;
    }
    Logger::info(QStringLiteral("[vdisplay-apply] device node removed"));
    return true;
}

/// Stage the package in the driver store and install it on OUR node only —
/// never on another node with the same hardware id (the owner's own VDD
/// keeps whatever driver it has).
bool installOnNode(HDEVINFO set, SP_DEVINFO_DATA& data, const QString& infPath, bool* reboot,
                   QString* error)
{
    const std::wstring inf = QDir::toNativeSeparators(infPath).toStdWString();
    if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
        *error = QStringLiteral("SetupCopyOEMInf: %1").arg(lastErrorText());
        return false;
    }
    BOOL needReboot = FALSE;
    // With no driver named, DiInstallDevice picks the best-ranked package in
    // the store for this node — the one just staged, or an identical or newer
    // copy of the same driver the owner installed before us.
    if (!DiInstallDevice(nullptr, set, &data, nullptr, 0, &needReboot)) {
        *error = QStringLiteral("DiInstallDevice: %1").arg(lastErrorText());
        return false;
    }
    *reboot = needReboot != FALSE;
    return true;
}

/// Delete the package from the driver store: find the oemNN.inf whose provider
/// is the driver's, then SetupUninstallOEMInf. Best effort — a package left in
/// the store is harmless once its nodes are gone.
void deletePackage()
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (set == INVALID_HANDLE_VALUE) return;
    if (SetupDiBuildDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER)) {
        SP_DRVINFO_DATA_W drv{};
        drv.cbSize = sizeof(drv);
        for (DWORD i = 0; SetupDiEnumDriverInfoW(set, nullptr, SPDIT_CLASSDRIVER, i, &drv); ++i) {
            if (QString::fromWCharArray(drv.ProviderName)
                    .compare(QStringLiteral("MikeTheTech"), Qt::CaseInsensitive) != 0)
                continue;
            DWORD need = 0;
            SetupDiGetDriverInfoDetailW(set, nullptr, &drv, nullptr, 0, &need);
            if (need == 0) continue;
            std::vector<BYTE> buf(need);
            auto* detail = reinterpret_cast<PSP_DRVINFO_DETAIL_DATA_W>(buf.data());
            detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
            if (!SetupDiGetDriverInfoDetailW(set, nullptr, &drv, detail, need, nullptr)) continue;
            const QString infName =
                QFileInfo(QString::fromWCharArray(detail->InfFileName)).fileName();
            if (!infName.startsWith(QLatin1String("oem"), Qt::CaseInsensitive)) continue;
            const std::wstring w = infName.toStdWString();
            if (SetupUninstallOEMInfW(w.c_str(), SUOI_FORCEDELETE, nullptr))
                Logger::info(
                    QStringLiteral("[vdisplay-apply] driver package %1 deleted").arg(infName));
            else
                Logger::warning(QStringLiteral("[vdisplay-apply] SetupUninstallOEMInf(%1): %2")
                                    .arg(infName, lastErrorText()));
        }
        SetupDiDestroyDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER);
    }
    SetupDiDestroyDeviceInfoList(set);
}

/// The modes the record says this project added to another VDD's file.
QList<VirtualDisplay::AddedMode> readAddedModes()
{
    QFile f(VirtualDisplay::addedModesPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return VirtualDisplay::parseAddedModes(QString::fromUtf8(f.readAll()));
}

/// The record rewritten, or removed when it holds nothing.
void writeAddedModes(const QList<VirtualDisplay::AddedMode>& modes)
{
    const QString path = VirtualDisplay::addedModesPath();
    if (modes.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        Logger::warning(QStringLiteral("[vdisplay-apply] cannot write %1").arg(path));
        return;
    }
    f.write(VirtualDisplay::addedModesText(modes).toUtf8());
}

/// @p xml with every mode the record names taken out; @p removed lists them.
QString withoutAddedModes(const QString& xml, QStringList* removed)
{
    QString out = xml;
    for (const VirtualDisplay::AddedMode& m : readAddedModes()) {
        bool changed = false;
        out =
            VirtualDisplay::settingsWithoutMode(out, m.width, m.height, m.refreshMilliHz, &changed);
        if (changed)
            removed->append(QStringLiteral("%1x%2 at %3 Hz")
                                .arg(m.width)
                                .arg(m.height)
                                .arg(VirtualDisplay::rateText(m.refreshMilliHz)));
    }
    return out;
}

/// The modes this project added to another VDD's file, taken out of it, and
/// the record cleared: the owner's list as it was before our first stream.
/// When the display is switched off, and when the package is uninstalled
/// beside another node. A file of ours needs nothing: it is rewritten whole.
void forgetAddedModes()
{
    const QString path = VirtualDisplay::settingsXmlPath();
    QFile f(path);
    QString current;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) current = QString::fromUtf8(f.readAll());
    f.close();
    if (!current.isEmpty() && !VirtualDisplay::isOurSettings(current)) {
        QStringList removed;
        const QString cleaned = withoutAddedModes(current, &removed);
        if (!removed.isEmpty()) {
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                Logger::warning(QStringLiteral("[vdisplay-apply] cannot write %1").arg(path));
                return; // the record stays, for the next try
            }
            f.write(cleaned.toUtf8());
            f.close();
            Logger::info(QStringLiteral("[vdisplay-apply] taken out of another VDD's mode list "
                                        "again: %1")
                             .arg(removed.join(QStringLiteral(", "))));
        }
    }
    writeAddedModes({});
}

/// The driver's mode list, holding @p width × @p height ahead of the rest.
///
/// The file is shared by every instance of the driver on the machine: one an
/// owner's own VDD wrote carries no marker of ours and is left exactly as it
/// is — the display then comes up at whatever that file lists — but for the
/// client's mode, added ahead of its list and recorded (addedModesPath), so
/// that the end of the stream, or the next activation, takes it out again.
/// Ours is rewritten whenever the wanted mode changes, and @p changed says
/// so, since the driver only reads this file when the device starts.
/// @p refreshMilliHz is the rate the display is to run at, 0 for the default.
/// @p gpu is the GPU the owner chose to draw it (Request::gpu): it goes into a
/// file of ours only — an owner's file keeps the GPU it names, which draws
/// that owner's own displays too.
bool writeSettings(int width, int height, int refreshMilliHz, const QString& gpu, bool* changed,
                   QString* error)
{
    *changed = false;
    const QString path = VirtualDisplay::settingsXmlPath();
    const QString wanted = VirtualDisplay::settingsXml(width, height, refreshMilliHz, gpu);
    QFile f(path);
    if (f.exists()) {
        QString current;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) current = QString::fromUtf8(f.readAll());
        f.close();
        if (current == wanted) return true;
        if (!current.isEmpty() && !VirtualDisplay::isOurSettings(current)) {
            // Someone else's file — an owner's own VDD, or an install of ours
            // from before the marker. It keeps every one of its modes; the
            // client's is added to the list, and nothing else is touched.
            //
            // But first, out with what an earlier stream of ours added and
            // nobody took out (a server stopped mid-stream, a display that did
            // not come up): the list must not grow from one client to the
            // next — two sizes at 240 Hz and the driver brought nothing up.
            QStringList removed;
            const QString cleaned = withoutAddedModes(current, &removed);
            if (!removed.isEmpty())
                Logger::info(QStringLiteral("[vdisplay-apply] left by an earlier stream, taken "
                                            "out of another VDD's mode list: %1")
                                 .arg(removed.join(QStringLiteral(", "))));
            // No size asked, but a rate the product never asked before
            // millihertz — a display matched to the client's screen, 239.744
            // for a 119.872 Hz one (`vddmatch`, plan glissade), or a bench's
            // MW_VDD_REFRESH: the default size at that rate, which the
            // owner's list does not hold. Windows applies no rate the driver
            // does not list. The product's own 240 and the default 120 are
            // left as they always were: whatever the owner's list says.
            const int milli = refreshMilliHz;
            const bool rateOnly = width <= 0 && milli > 0 &&
                                  milli != VirtualDisplay::kRateMax * VirtualDisplay::kMilliHz &&
                                  milli != VirtualDisplay::kRefreshHz * VirtualDisplay::kMilliHz;
            const int addW = rateOnly ? VirtualDisplay::kWidth : width;
            const int addH = rateOnly ? VirtualDisplay::kHeight : height;
            // The mode as settingsWithMode() spells it, for the record.
            VirtualDisplay::AddedMode mode{addW, addH, milli};
            const bool sized = VirtualDisplay::normaliseMode(mode.width, mode.height);
            if (!VirtualDisplay::normaliseRate(mode.refreshMilliHz))
                mode.refreshMilliHz = VirtualDisplay::kRefreshHz * VirtualDisplay::kMilliHz;
            bool added = false;
            const QString grown =
                VirtualDisplay::settingsWithMode(cleaned, addW, addH, milli, &added);
            writeAddedModes(added && sized ? QList<VirtualDisplay::AddedMode>{mode}
                                           : QList<VirtualDisplay::AddedMode>{});
            if (grown == current) {
                Logger::info(QStringLiteral("[vdisplay-apply] %1 belongs to another VDD — left as "
                                            "it is")
                                 .arg(path));
                return true;
            }
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                *error = QStringLiteral("cannot write %1").arg(path);
                return false;
            }
            f.write(grown.toUtf8());
            f.close();
            *changed = true;
            if (added)
                Logger::info(QStringLiteral("[vdisplay-apply] %1x%2 at %3 Hz added to another "
                                            "VDD's mode list in %4 — its own modes kept, ours "
                                            "taken out at the end")
                                 .arg(mode.width)
                                 .arg(mode.height)
                                 .arg(VirtualDisplay::rateText(mode.refreshMilliHz))
                                 .arg(path));
            return true;
        }
    } else if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("cannot create %1").arg(path);
        return false;
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    f.write(wanted.toUtf8());
    f.close();
    *changed = true;
    Logger::info(QStringLiteral("[vdisplay-apply] mode list rewritten with %1x%2 at %3 Hz first, "
                                "drawn by %4")
                     .arg(width > 0 ? width : VirtualDisplay::kWidth)
                     .arg(height > 0 ? height : VirtualDisplay::kHeight)
                     .arg(refreshMilliHz > 0 ? VirtualDisplay::rateText(refreshMilliHz)
                                             : QString::number(VirtualDisplay::kRefreshHz))
                     .arg(gpu.isEmpty() ? QStringLiteral("the driver's choice of GPU") : gpu));
    return true;
}

// ── Stage "driver": the elevated half ───────────────────────────────────────

int stageDriver(Context& ctx)
{
    using Action = VirtualDisplay::Request::Action;
    const QString stage = QStringLiteral("driver");
    if (!VirtualDisplay::processElevated())
        return failAt(ctx, stage, QStringLiteral("this process is not elevated"));

    DisplayNodes nodes;
    if (!nodes.valid())
        return failAt(ctx, stage, QStringLiteral("SetupDiGetClassDevs: %1").arg(lastErrorText()));
    QString error;

    switch (ctx.request.action) {
    case Action::Install: {
        if (nodes.ours()) {
            Logger::info(QStringLiteral("[vdisplay-apply] \"%1\" is already installed")
                             .arg(VirtualDisplay::displayName()));
            ctx.result.ok = true;
            ctx.result.stage = QStringLiteral("done");
            return finish(ctx);
        }
        // The files as the installer laid them out, re-hashed against the
        // constants compiled into THIS exe before SetupAPI sees them.
        const QString dir = VirtualDisplay::bundledDriverDir();
        QString infPath, catPath;
        for (const VirtualDisplay::DriverFile& f : VirtualDisplay::driverFiles()) {
            const QString path = dir + QLatin1Char('/') + QLatin1String(f.name);
            if (DriverPackage::fileSha256(path) != QLatin1String(f.sha256))
                return failAt(ctx, stage,
                              QStringLiteral("%1 is missing or does not match its pinned hash")
                                  .arg(QLatin1String(f.name)));
            if (path.endsWith(QLatin1String(".inf"), Qt::CaseInsensitive)) infPath = path;
            if (path.endsWith(QLatin1String(".cat"), Qt::CaseInsensitive)) catPath = path;
        }
        // The driver's settings: the default mode list, and only when nobody
        // wrote any. An owner's own VDD configured that file, and ours takes
        // the modes it lists.
        bool wrote = false;
        if (!writeSettings(0, 0, 0, QString(), &wrote, &error)) return failAt(ctx, stage, error);

        DriverPackage::PublisherTrust trust;
        if (!DriverPackage::trustCatalogSigner(catPath, VirtualDisplay::catalogSignerThumbprint(),
                                               false, "[vdisplay-apply]", trust, &error))
            return failAt(ctx, stage, error);
        SP_DEVINFO_DATA data{};
        if (!createOurNode(nodes.set, data, &error)) return failAt(ctx, stage, error);
        bool reboot = false;
        if (!installOnNode(nodes.set, data, infPath, &reboot, &error)) {
            // Leave no orphan node behind a failed install.
            QString ignored;
            removeNode(nodes.set, data, &ignored);
            return failAt(ctx, stage, error);
        }
        // The name again, now that the driver is bound: installing the
        // package rewrites the node's description from the INF and, as
        // measured on the bench, drops the friendly name set before it. This
        // write is the one that lasts — and the one every lookup relies on.
        if (!setFriendlyName(nodes.set, data, &error)) {
            QString ignored;
            removeNode(nodes.set, data, &ignored);
            return failAt(ctx, stage, error);
        }
        // Off by default: the card turns it on.
        if (!setEnabled(nodes.set, data, false, &error))
            Logger::warning(QStringLiteral("[vdisplay-apply] %1").arg(error));
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        ctx.result.rebootRequired = reboot;
        Logger::info(QStringLiteral("[vdisplay-apply] \"%1\" installed%2")
                         .arg(VirtualDisplay::displayName(),
                              reboot ? QStringLiteral(" (reboot required)") : QString()));
        return finish(ctx);
    }

    case Action::Uninstall: {
        bool othersRemain = false;
        bool ok = true;
        for (DisplayNodes::Node n : nodes.driverNodes()) {
            if (!n.ours) {
                othersRemain = true;
                continue;
            }
            if (!removeNode(nodes.set, n.data, &error)) ok = false;
        }
        if (!ok) return failAt(ctx, stage, error);
        if (!othersRemain) {
            deletePackage();
            QFile::remove(VirtualDisplay::settingsXmlPath());
        } else {
            Logger::info(QStringLiteral(
                "[vdisplay-apply] another node uses the driver — package and settings kept"));
            forgetAddedModes();
        }
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    case Action::Activate:
    case Action::Deactivate: {
        auto ours = nodes.ours();
        if (!ours)
            return failAt(ctx, stage,
                          QStringLiteral("\"%1\" is not installed on this machine")
                              .arg(VirtualDisplay::displayName()));
        const bool enable = ctx.request.action == Action::Activate;
        // The mode the client asked for has to be in the driver's list before
        // the device starts, because that is when the driver reads it. The
        // list changing under a device already running — the display was left
        // on by a stream that just ended — is what a restart is for.
        if (enable) {
            bool listChanged = false;
            if (!writeSettings(ctx.request.width, ctx.request.height, ctx.request.refreshMilliHz,
                               ctx.request.gpu, &listChanged, &error)) {
                Logger::warning(
                    QStringLiteral("[vdisplay-apply] %1 — the default mode stands").arg(error));
            } else if (listChanged && nodeIsEnabled(*ours)) {
                QString restartError;
                if (setEnabled(nodes.set, *ours, false, &restartError) &&
                    setEnabled(nodes.set, *ours, true, &restartError))
                    Logger::info(QStringLiteral(
                        "[vdisplay-apply] device node restarted to re-read the mode list"));
                else
                    Logger::warning(QStringLiteral("[vdisplay-apply] node not restarted: %1")
                                        .arg(restartError));
            }
        }
        if (nodeIsEnabled(*ours) == enable) {
            Logger::info(QStringLiteral("[vdisplay-apply] device node already %1")
                             .arg(enable ? QStringLiteral("enabled") : QStringLiteral("disabled")));
        } else if (!setEnabled(nodes.set, *ours, enable, &error)) {
            return failAt(ctx, stage, error);
        }
        // Off: the client's mode leaves the owner's list with the stream. The
        // driver read it when the device started; a device stopped reads
        // nothing until the next activation adds what that client needs.
        if (!enable) forgetAddedModes();
        ctx.result.ok = true;
        ctx.result.stage = stage;
        return kExitOk; // the caller decides whether a mode stage follows
    }

    case Action::OthersBack: break; // the desktop's business, never the node's
    }
    return failAt(ctx, stage, QStringLiteral("unknown action"));
}

// ── Stage "mode": the desktop half ──────────────────────────────────────────

struct ActivePath
{
    LUID adapterId{};
    UINT32 targetId = 0;
    QString gdiName;    // "\\.\DISPLAY3"
    QString devicePath; // "\\?\DISPLAY#MTT1337#1&15ecd195&0&UID256#{…}"
    bool primary = false;
    bool active = true;
    DISPLAYCONFIG_PATH_INFO raw{}; // as Windows listed it
};

/// Every display path Windows lists for @p flags (QDC_ONLY_ACTIVE_PATHS or
/// QDC_ALL_PATHS, the latter narrowed to attached monitors), with what
/// identifies it on both sides: the GDI name (mode calls) and the monitor
/// device path (stable identity).
std::vector<ActivePath> listPaths(UINT32 flags)
{
    std::vector<ActivePath> out;
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(flags, &nPaths, &nModes) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(flags, &nPaths, paths.data(), &nModes, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return out;

    for (UINT32 i = 0; i < nPaths; ++i) {
        const DISPLAYCONFIG_PATH_INFO& p = paths[i];
        if (flags == QDC_ALL_PATHS && !p.targetInfo.targetAvailable) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;

        ActivePath a;
        a.raw = p;
        a.active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        a.adapterId = p.targetInfo.adapterId;
        a.targetId = p.targetInfo.id;
        a.devicePath = QString::fromWCharArray(tn.monitorDevicePath);
        if (a.active) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof(sn);
            sn.header.adapterId = p.sourceInfo.adapterId;
            sn.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
            a.gdiName = QString::fromWCharArray(sn.viewGdiDeviceName);
        }
        const UINT32 srcIdx = p.sourceInfo.modeInfoIdx;
        if (a.active && srcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && srcIdx < modes.size() &&
            modes[srcIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const POINTL& pos = modes[srcIdx].sourceMode.position;
            a.primary = pos.x == 0 && pos.y == 0;
        }
        out.push_back(a);
    }
    return out;
}

std::vector<ActivePath> activePaths()
{
    return listPaths(QDC_ONLY_ACTIVE_PATHS);
}

/// The monitor ids hanging off our node, spelled like a device path.
QStringList ourMonitorIds()
{
    QStringList out;
    DisplayNodes nodes;
    if (!nodes.valid()) return out;
    const auto ours = nodes.ours();
    if (!ours) return out;
    DEVINST child = 0;
    if (CM_Get_Child(&child, ours->DevInst, 0) != CR_SUCCESS) return out;
    for (;;) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(child, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            QString s = QString::fromWCharArray(id).toLower();
            s.replace(QLatin1Char('\\'), QLatin1Char('#'));
            out.append(s);
        }
        DEVINST sibling = 0;
        if (CM_Get_Sibling(&sibling, child, 0) != CR_SUCCESS) break;
        child = sibling;
    }
    return out;
}

/// Our monitor among the paths listed for @p flags; QDC_ALL_PATHS finds it
/// attached but switched off, QDC_ONLY_ACTIVE_PATHS only once it is on.
std::optional<ActivePath> findOurTarget(UINT32 flags = QDC_ONLY_ACTIVE_PATHS)
{
    const QStringList ids = ourMonitorIds();
    if (ids.isEmpty()) return std::nullopt;
    for (const ActivePath& p : listPaths(flags)) {
        const QString path = p.devicePath.toLower();
        for (const QString& id : ids)
            if (path.contains(id)) return p;
    }
    return std::nullopt;
}

// ── The display layout before ours: saved, kept, restored ──────────────────
//
// Windows left to itself answers a new display by activating every display
// it can find — including one the owner had set to "disconnect" in
// Settings — and when the new one goes, the extra one stays on. So the
// active configuration (paths + modes, as the CCD API keeps it) is saved
// before our node is enabled, imposed again once our display is up (plus
// ours), and applied back, without ours, when the stream ends: what was on
// is on, what was off is off, and the primary is the one it was.

struct Topology
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

struct TopologyHeader
{
    char magic[4];
    quint32 version;
    quint32 nPaths;
    quint32 nModes;
    quint32 pathSize;
    quint32 modeSize;
};
constexpr char kTopologyMagic[4] = {'M', 'W', 'V', 'T'};
constexpr quint32 kTopologyVersion = 1;
constexpr quint32 kTopologyMaxPaths = 64;
constexpr quint32 kTopologyMaxModes = 256;

QString topologyPath(const Context& ctx)
{
    return ctx.dir + QStringLiteral("/topology.bin");
}

bool sameTarget(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b)
{
    return a.targetInfo.adapterId.LowPart == b.targetInfo.adapterId.LowPart &&
           a.targetInfo.adapterId.HighPart == b.targetInfo.adapterId.HighPart &&
           a.targetInfo.id == b.targetInfo.id;
}

/// The active configuration, to the staging directory. The directory is the
/// user's and this process may be elevated: what is read back only ever
/// reaches SetDisplayConfig, a call any process on the desktop can make.
bool saveTopology(const Context& ctx, QString* error)
{
    Topology t;
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) {
        *error = QStringLiteral("GetDisplayConfigBufferSizes failed");
        return false;
    }
    t.paths.resize(nPaths);
    t.modes.resize(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, t.paths.data(), &nModes, t.modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        *error = QStringLiteral("QueryDisplayConfig failed");
        return false;
    }
    t.paths.resize(nPaths);
    t.modes.resize(nModes);
    if (nPaths == 0 || nPaths > kTopologyMaxPaths || nModes > kTopologyMaxModes) {
        *error = QStringLiteral("no active display to remember");
        return false;
    }

    TopologyHeader h{};
    memcpy(h.magic, kTopologyMagic, sizeof(h.magic));
    h.version = kTopologyVersion;
    h.nPaths = nPaths;
    h.nModes = nModes;
    h.pathSize = sizeof(DISPLAYCONFIG_PATH_INFO);
    h.modeSize = sizeof(DISPLAYCONFIG_MODE_INFO);
    QFile f(topologyPath(ctx));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("cannot write %1").arg(f.fileName());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(t.paths.data()),
            qint64(nPaths * sizeof(DISPLAYCONFIG_PATH_INFO)));
    f.write(reinterpret_cast<const char*>(t.modes.data()),
            qint64(nModes * sizeof(DISPLAYCONFIG_MODE_INFO)));
    return true;
}

std::optional<Topology> loadTopology(const Context& ctx)
{
    QFile f(topologyPath(ctx));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray raw = f.readAll();
    TopologyHeader h{};
    if (raw.size() < qsizetype(sizeof(h))) return std::nullopt;
    memcpy(&h, raw.constData(), sizeof(h));
    if (memcmp(h.magic, kTopologyMagic, sizeof(h.magic)) != 0 || h.version != kTopologyVersion ||
        h.pathSize != sizeof(DISPLAYCONFIG_PATH_INFO) ||
        h.modeSize != sizeof(DISPLAYCONFIG_MODE_INFO) || h.nPaths == 0 ||
        h.nPaths > kTopologyMaxPaths || h.nModes > kTopologyMaxModes)
        return std::nullopt;
    const qsizetype expected = qsizetype(sizeof(h) + h.nPaths * h.pathSize + h.nModes * h.modeSize);
    if (raw.size() != expected) return std::nullopt;

    Topology t;
    t.paths.resize(h.nPaths);
    t.modes.resize(h.nModes);
    memcpy(t.paths.data(), raw.constData() + sizeof(h), h.nPaths * h.pathSize);
    memcpy(t.modes.data(), raw.constData() + sizeof(h) + h.nPaths * h.pathSize,
           h.nModes * h.modeSize);
    for (const DISPLAYCONFIG_PATH_INFO& p : t.paths) {
        for (const UINT32 idx : {p.sourceInfo.modeInfoIdx, p.targetInfo.modeInfoIdx})
            if (idx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && idx >= h.nModes) return std::nullopt;
    }
    return t;
}

/// Whether every display of @p t is still attached — a saved layout from
/// before a reboot names adapters that no longer exist.
bool topologyStillAttached(const Topology& t)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return false;
    paths.resize(nPaths);
    for (const DISPLAYCONFIG_PATH_INFO& want : t.paths) {
        bool found = false;
        for (const DISPLAYCONFIG_PATH_INFO& have : paths)
            if (sameTarget(want, have) && (have.targetInfo.targetAvailable != 0)) found = true;
        if (!found) return false;
    }
    return true;
}

/// Apply @p t as the whole configuration: a display it does not name goes off.
/// Not saved to Windows' database: ours is still attached at this point, and
/// an entry "ours off" for this set of displays would be what Windows replays
/// the next time the node comes up — the display would never appear.
bool applyTopology(const Topology& t, QString* error)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths = t.paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes = t.modes;
    const LONG rc =
        SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()), modes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    if (rc != ERROR_SUCCESS) {
        *error = QStringLiteral("SetDisplayConfig: %1").arg(lastErrorText(DWORD(rc)));
        return false;
    }
    return true;
}

/// Query the active display configuration, let @p edit change it, apply it —
/// and save it to Windows' database when @p save (VirtualDisplay::savesLayout).
/// The CCD API (what Windows Settings uses), not ChangeDisplaySettingsEx:
/// on the bench an indirect display just brought up took neither a mode nor
/// a position through GDI — success returned, nothing changed.
template <typename Edit> bool applyDisplayConfig(Edit edit, bool save, QString* error)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) {
        *error = QStringLiteral("GetDisplayConfigBufferSizes failed");
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        *error = QStringLiteral("QueryDisplayConfig failed");
        return false;
    }
    paths.resize(nPaths);
    modes.resize(nModes);
    if (!edit(paths, modes, error)) return false;
    const LONG rc =
        SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()), modes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
                             (save ? SDC_SAVE_TO_DATABASE : 0));
    if (rc != ERROR_SUCCESS) {
        *error = QStringLiteral("SetDisplayConfig: %1").arg(lastErrorText(DWORD(rc)));
        return false;
    }
    return true;
}

/// The index of the active path whose monitor device path is @p devicePath.
int pathIndexFor(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths, const QString& devicePath)
{
    for (size_t i = 0; i < paths.size(); ++i) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        if (QString::fromWCharArray(tn.monitorDevicePath)
                .compare(devicePath, Qt::CaseInsensitive) == 0)
            return int(i);
    }
    return -1;
}

/// Our mode on the display at @p devicePath: the source (desktop) size, and
/// the target left to the driver at @p refreshMilliHz. @p width × @p height is
/// the client's size when it asked for one, the default mode otherwise, and
/// @p refreshMilliHz the stream's choice or the default — either way the
/// driver's list holds the pair by now (writeSettings()). The rate goes as the
/// exact fraction the driver lists (rateFraction: 29968/125 for 239.744), the
/// only one SetDisplayConfig finds.
bool setMode(const QString& devicePath, int width, int height, int refreshMilliHz, bool save,
             QString* error)
{
    const VirtualDisplay::RateFraction rate = VirtualDisplay::rateFraction(refreshMilliHz);
    return applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString* err) {
            const int i = pathIndexFor(paths, devicePath);
            if (i < 0) {
                *err = QStringLiteral("the display is not in the active configuration");
                return false;
            }
            DISPLAYCONFIG_PATH_INFO& p = paths[size_t(i)];
            const UINT32 src = p.sourceInfo.modeInfoIdx;
            if (src == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || src >= modes.size()) {
                *err = QStringLiteral("the display has no source mode");
                return false;
            }
            modes[src].sourceMode.width = UINT32(width);
            modes[src].sourceMode.height = UINT32(height);
            p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            p.targetInfo.refreshRate.Numerator = UINT32(rate.numerator);
            p.targetInfo.refreshRate.Denominator = UINT32(rate.denominator);
            p.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
            p.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
            return true;
        },
        save, error);
}

/// Switch on the attached, inactive display @p attached (a QDC_ALL_PATHS
/// entry): its path joins the active ones, modes and position left to
/// Windows. Needed when Windows' own answer to the monitor is "off" — its
/// database remembers the last layout for this set of displays, and that is
/// what a stream's end leaves.
bool switchOn(const ActivePath& attached, bool save, QString* error)
{
    return applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString*) {
            Q_UNUSED(modes);
            DISPLAYCONFIG_PATH_INFO p = attached.raw;
            // A source already driving another display on that adapter
            // would make a clone: take a free one.
            for (;;) {
                bool taken = false;
                for (const DISPLAYCONFIG_PATH_INFO& q : paths)
                    if (q.sourceInfo.adapterId.LowPart == p.sourceInfo.adapterId.LowPart &&
                        q.sourceInfo.adapterId.HighPart == p.sourceInfo.adapterId.HighPart &&
                        q.sourceInfo.id == p.sourceInfo.id)
                        taken = true;
                if (!taken || p.sourceInfo.id >= 15) break;
                ++p.sourceInfo.id;
            }
            p.flags = DISPLAYCONFIG_PATH_ACTIVE;
            p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            p.targetInfo.statusFlags = 0;
            paths.push_back(p);
            return true;
        },
        save, error);
}

/// Switch off every active display that is neither in @p t nor ours at
/// @p devicePath — what Windows lit up on its own when ours appeared.
/// @return how many went off; -1 on failure.
int pruneToTopology(const Topology& t, const QString& devicePath, bool save, QString* error)
{
    int pruned = 0;
    const bool ok = applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString*) {
            Q_UNUSED(modes);
            const int ours = pathIndexFor(paths, devicePath);
            std::vector<DISPLAYCONFIG_PATH_INFO> keep;
            for (size_t i = 0; i < paths.size(); ++i) {
                bool wanted = int(i) == ours;
                for (const DISPLAYCONFIG_PATH_INFO& p : t.paths)
                    if (sameTarget(p, paths[i])) wanted = true;
                if (wanted)
                    keep.push_back(paths[i]);
                else
                    ++pruned;
            }
            paths = keep;
            return true;
        },
        save, error);
    return ok ? pruned : -1;
}

/// The monitor device path of @p p's target, empty when Windows does not say.
QString targetDevicePath(const DISPLAYCONFIG_PATH_INFO& p)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = p.targetInfo.adapterId;
    tn.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) return QString();
    return QString::fromWCharArray(tn.monitorDevicePath);
}

/// @p p appended to @p out, with the modes it names copied from @p modes and
/// its indices made to point at the copies; @p copied maps an index of
/// @p modes to its copy's, so a mode two paths share is copied once.
void addPath(Topology& out, DISPLAYCONFIG_PATH_INFO p,
             const std::vector<DISPLAYCONFIG_MODE_INFO>& modes, std::vector<UINT32>& copied)
{
    copied.resize(modes.size(), DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
    for (UINT32* idx : {&p.sourceInfo.modeInfoIdx, &p.targetInfo.modeInfoIdx}) {
        if (*idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) continue;
        if (*idx >= modes.size()) {
            *idx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            continue;
        }
        if (copied[*idx] == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            copied[*idx] = UINT32(out.modes.size());
            out.modes.push_back(modes[*idx]);
        }
        *idx = copied[*idx];
    }
    out.paths.push_back(p);
}

/// The layout from before ours, @p t, imposed again now that ours at
/// @p devicePath is up: every other display in the mode and at the place it
/// had, ours on their right (placeBeside), nothing else on — and saved to
/// Windows' database as this set of displays' layout. Windows brings a set up
/// from that database, with the modes each display had the last time it was
/// on: an owner's own VDD on the same driver came back at 800×600 at 30 Hz,
/// the first mode of its list, every time ours came on, and every change of
/// ours saved it again at that (N95, 16/10/2026). Saved this way, the next
/// activation brings it up as the owner left it. Switches off, as
/// pruneToTopology() does, whatever Windows lit up beside ours.
bool imposeTopology(const Topology& t, const QString& devicePath, QString* error)
{
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) {
        *error = QStringLiteral("GetDisplayConfigBufferSizes failed");
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        *error = QStringLiteral("QueryDisplayConfig failed");
        return false;
    }
    paths.resize(nPaths);
    modes.resize(nModes);
    const int i = pathIndexFor(paths, devicePath);
    if (i < 0) {
        *error = QStringLiteral("the display is not in the active configuration");
        return false;
    }
    const DISPLAYCONFIG_PATH_INFO& ours = paths[size_t(i)];
    const UINT32 ourSource = ours.sourceInfo.modeInfoIdx;
    if (ourSource == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || ourSource >= modes.size() ||
        modes[ourSource].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        *error = QStringLiteral("the display has no source mode");
        return false;
    }

    Topology out;
    std::vector<UINT32> copied;
    QList<VirtualDisplay::DesktopRect> others;
    for (const DISPLAYCONFIG_PATH_INFO& p : t.paths) {
        // Ours as an earlier stream left it on: it comes as it is now.
        if (sameTarget(p, ours) ||
            targetDevicePath(p).compare(devicePath, Qt::CaseInsensitive) == 0)
            continue;
        if (p.sourceInfo.adapterId.LowPart == ours.sourceInfo.adapterId.LowPart &&
            p.sourceInfo.adapterId.HighPart == ours.sourceInfo.adapterId.HighPart &&
            p.sourceInfo.id == ours.sourceInfo.id) {
            *error = QStringLiteral("the display's source drives another display already");
            return false;
        }
        addPath(out, p, t.modes, copied);
        const UINT32 src = p.sourceInfo.modeInfoIdx;
        if (src != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && src < t.modes.size() &&
            t.modes[src].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const DISPLAYCONFIG_SOURCE_MODE& m = t.modes[src].sourceMode;
            others.append({int(m.position.x), int(m.position.y), int(m.width), int(m.height)});
        }
    }
    std::vector<DISPLAYCONFIG_MODE_INFO> ourModes = modes;
    const QPoint at = VirtualDisplay::placeBeside(others);
    ourModes[ourSource].sourceMode.position.x = at.x();
    ourModes[ourSource].sourceMode.position.y = at.y();
    std::vector<UINT32> ourCopied;
    addPath(out, ours, ourModes, ourCopied);

    const LONG rc = SetDisplayConfig(
        UINT32(out.paths.size()), out.paths.data(), UINT32(out.modes.size()), out.modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (rc != ERROR_SUCCESS) {
        *error = QStringLiteral("SetDisplayConfig: %1").arg(lastErrorText(DWORD(rc)));
        return false;
    }
    return true;
}

/// Make the display at @p devicePath the primary one: every desktop origin
/// shifted so that one lands on (0,0), in one configuration change.
bool makePrimary(const QString& devicePath, bool save, QString* error)
{
    return applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString* err) {
            const int i = pathIndexFor(paths, devicePath);
            if (i < 0) {
                *err = QStringLiteral("the display is not in the active configuration");
                return false;
            }
            const UINT32 src = paths[size_t(i)].sourceInfo.modeInfoIdx;
            if (src == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || src >= modes.size()) {
                *err = QStringLiteral("the display has no source mode");
                return false;
            }
            const POINTL origin = modes[src].sourceMode.position;
            if (origin.x == 0 && origin.y == 0) return true; // already the primary
            for (DISPLAYCONFIG_MODE_INFO& m : modes) {
                if (m.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
                m.sourceMode.position.x -= origin.x;
                m.sourceMode.position.y -= origin.y;
            }
            return true;
        },
        save, error);
}

/// Every display but ours at @p devicePath off (Request::exclusive), never
/// saved (VirtualDisplay::savesLayout): Windows' database keeps them on, and
/// replays that the moment ours goes, whoever switches it off.
/// @return how many went off; -1 on failure.
int keepOnlyOurs(const QString& devicePath, QString* error)
{
    int off = 0;
    const bool ok = applyDisplayConfig(
        [&](std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
            std::vector<DISPLAYCONFIG_MODE_INFO>& modes, QString* err) {
            Q_UNUSED(modes);
            const int ours = pathIndexFor(paths, devicePath);
            if (ours < 0) {
                *err = QStringLiteral("the display is not in the active configuration");
                return false;
            }
            off = int(paths.size()) - 1;
            paths = {paths[size_t(ours)]};
            return true;
        },
        VirtualDisplay::savesLayout(VirtualDisplay::LayoutStep::Exclusive, true), error);
    return ok ? off : -1;
}

/// The other displays are off for an exclusive stream (keepOnlyOurs): a
/// marker beside the saved layout, so that the next process — a mode change
/// mid-stream, the stream's end, a server restarted after a crash — knows
/// that what is active is no layout to save or to remember.
QString othersOffPath(const Context& ctx)
{
    return ctx.dir + QStringLiteral("/others-off");
}

bool othersOff(const Context& ctx)
{
    return QFile::exists(othersOffPath(ctx));
}

void setOthersOff(const Context& ctx, bool off)
{
    if (!off) {
        QFile::remove(othersOffPath(ctx));
        return;
    }
    QFile f(othersOffPath(ctx));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        Logger::warning(QStringLiteral("[vdisplay-apply] cannot write %1").arg(f.fileName()));
}

// ── Stage "snapshot": the desktop half that runs before the node goes on ──

int stageSnapshot(Context& ctx)
{
    using Action = VirtualDisplay::Request::Action;
    ctx.result.ok = true;
    ctx.result.stage = QStringLiteral("snapshot");
    if (ctx.request.action != Action::Activate) return kExitOk;
    QString error;
    if (othersOff(ctx)) {
        // A new mode for an exclusive stream's display: what is active is ours
        // alone, and the layout to come back to is the one already saved.
        if (loadTopology(ctx)) {
            Logger::info(QStringLiteral("[vdisplay-apply] the other displays are off — the "
                                        "layout from before is kept"));
        } else {
            Logger::warning(QStringLiteral("[vdisplay-apply] the other displays are off and no "
                                           "layout from before is saved"));
        }
        return kExitOk;
    }
    if (saveTopology(ctx, &error)) {
        Logger::info(QStringLiteral("[vdisplay-apply] display layout saved"));
    } else {
        // Not fatal: the primary alone is restored then (the way back below).
        QFile::remove(topologyPath(ctx));
        Logger::warning(QStringLiteral("[vdisplay-apply] display layout not saved: %1").arg(error));
    }
    return kExitOk;
}

int stageMode(Context& ctx)
{
    using Action = VirtualDisplay::Request::Action;
    const QString stage = QStringLiteral("mode");

    if (ctx.request.action == Action::Deactivate) {
        ctx.result.ok = true;
        ctx.result.stage = stage;
        const bool wereOff = othersOff(ctx);
        // The layout from before ours, applied while ours still exists: that
        // switches ours off, puts every other display back where it was and
        // gives the primary role back, in one change. The node goes next.
        if (const auto saved = loadTopology(ctx)) {
            QString error;
            if (!topologyStillAttached(*saved)) {
                Logger::info(QStringLiteral(
                    "[vdisplay-apply] the saved display layout names a display that is "
                    "gone — restoring the primary only"));
            } else if (applyTopology(*saved, &error)) {
                Logger::info(QStringLiteral("[vdisplay-apply] display layout restored"));
                QFile::remove(topologyPath(ctx));
                setOthersOff(ctx, false);
                return kExitOk; // the driver stage (disable) follows
            } else {
                Logger::warning(
                    QStringLiteral("[vdisplay-apply] display layout not restored: %1").arg(error));
            }
            QFile::remove(topologyPath(ctx));
        }
        if (wereOff) {
            // An exclusive stream's displays, without the layout to put them
            // back as they were: Windows' own extended layout for the displays
            // attached, which never saw them off (savesLayout).
            const LONG rc =
                SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
            if (rc == ERROR_SUCCESS)
                Logger::info(QStringLiteral("[vdisplay-apply] the other displays back on, "
                                            "as Windows extends them"));
            else
                Logger::warning(
                    QStringLiteral("[vdisplay-apply] the other displays not back on: %1")
                        .arg(lastErrorText(DWORD(rc))));
            setOthersOff(ctx, false);
        }
        // The previous primary back in its role, while ours still exists —
        // Windows would otherwise pick one itself when ours goes.
        if (!ctx.request.restorePrimary.isEmpty()) {
            bool found = false;
            for (const ActivePath& p : activePaths()) {
                if (p.devicePath.compare(ctx.request.restorePrimary, Qt::CaseInsensitive) != 0)
                    continue;
                found = true;
                QString error;
                if (!p.primary && !makePrimary(p.devicePath,
                                               VirtualDisplay::savesLayout(
                                                   VirtualDisplay::LayoutStep::Primary, false),
                                               &error))
                    Logger::warning(
                        QStringLiteral("[vdisplay-apply] previous primary not restored: %1")
                            .arg(error));
            }
            if (!found)
                Logger::info(QStringLiteral(
                    "[vdisplay-apply] the previous primary display is gone — Windows picks"));
        }
        return kExitOk; // the driver stage (disable) follows
    }
    // The owner left an exclusive stream and guests still watch ours: the other
    // displays back, ours keeping its mode and the primary role, the node left
    // alone. The guests' capture restarts on its own, as for any display change.
    if (ctx.request.action == Action::OthersBack) {
        if (othersOff(ctx)) {
            const auto target = findOurTarget();
            if (!target) return failAt(ctx, stage, QStringLiteral("the display is not on"));
            QString error;
            bool back = false;
            if (const auto saved = loadTopology(ctx); saved && topologyStillAttached(*saved)) {
                back = imposeTopology(*saved, target->devicePath, &error);
                if (!back)
                    Logger::warning(
                        QStringLiteral("[vdisplay-apply] the layout from before not imposed: %1")
                            .arg(error));
            }
            if (!back) {
                const LONG rc =
                    SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
                back = rc == ERROR_SUCCESS;
                if (!back)
                    return failAt(ctx, stage,
                                  QStringLiteral("the other displays not back on: %1")
                                      .arg(lastErrorText(DWORD(rc))));
            }
            setOthersOff(ctx, false);
            Logger::info(QStringLiteral("[vdisplay-apply] the owner left: the other displays "
                                        "back on, the guests' stream goes on"));
            if (const auto again = findOurTarget();
                again && !makePrimary(again->devicePath,
                                      VirtualDisplay::savesLayout(
                                          VirtualDisplay::LayoutStep::Primary, false),
                                      &error))
                Logger::warning(QStringLiteral("[vdisplay-apply] not made primary: %1").arg(error));
        }
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }
    if (ctx.request.action != Action::Activate) {
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    // The monitor appears a moment after the driver starts; ten seconds is
    // generous for a UMDF driver and a single monitor. Whether Windows then
    // switches it on is its database's call — attached and off, we do it.
    using VirtualDisplay::LayoutStep;
    using VirtualDisplay::savesLayout;
    // An exclusive stream's display taking a new mode: the others are off.
    bool off = othersOff(ctx);
    std::optional<ActivePath> target;
    QElapsedTimer wait;
    wait.start();
    bool switchedOn = false;
    QString error;
    while (!(target = findOurTarget()) && wait.elapsed() < 10 * 1000) {
        if (!switchedOn) {
            if (const auto attached = findOurTarget(QDC_ALL_PATHS)) {
                switchedOn = true;
                if (switchOn(*attached, savesLayout(LayoutStep::Mode, off), &error))
                    Logger::info(QStringLiteral(
                        "[vdisplay-apply] the virtual display was attached but off: switched on"));
                else
                    Logger::warning(
                        QStringLiteral("[vdisplay-apply] cannot switch the virtual display on: %1")
                            .arg(error));
            }
        }
        QThread::msleep(250);
    }
    if (!target) {
        if (ctx.result.rebootRequired) {
            ctx.result.ok = true;
            ctx.result.stage = QStringLiteral("done");
            return finish(ctx);
        }
        // Nothing came of the device the driver stage just switched on: it
        // goes back off, as it was before this activation. Left on, it stays
        // enabled with no display behind it until the server's next start
        // turns it off (seen on the bench, 29/09/2026: a 500 Hz mode the
        // driver would not bring up, then the next launch refused as well).
        DisplayNodes nodes;
        if (auto ours = nodes.valid() ? nodes.ours() : std::nullopt; ours && nodeIsEnabled(*ours)) {
            QString disableError;
            if (!setEnabled(nodes.set, *ours, false, &disableError))
                Logger::warning(
                    QStringLiteral("[vdisplay-apply] the device stays on: %1").arg(disableError));
        }
        // Nor does the mode it was asked for stay in an owner's list.
        forgetAddedModes();
        // The server tries again closer to the default mode
        // (VirtualDisplay::fallbackTowardsDefault).
        return failAt(ctx, stage, VirtualDisplay::notAppearedError());
    }
    ctx.result.display = target->gdiName;

    // Who holds the primary role right now, for the way back.
    for (const ActivePath& p : activePaths())
        if (p.primary && p.devicePath.compare(target->devicePath, Qt::CaseInsensitive) != 0)
            ctx.result.previousPrimary = p.devicePath;

    // Only what was on before, as it was, plus ours: Windows may have switched
    // on a display the owner keeps disconnected, and brought the others up in
    // the modes its database remembers for this set (imposeTopology). When
    // that layout cannot be imposed, the extra displays go off all the same.
    // An exclusive stream's display taking a new mode keeps the others off:
    // imposing the layout from before would light them up for a moment.
    const bool stayExclusive = off && ctx.request.exclusive;
    bool imposed = false;
    if (const auto saved = loadTopology(ctx);
        !stayExclusive && saved && topologyStillAttached(*saved)) {
        imposed = imposeTopology(*saved, target->devicePath, &error);
        off = VirtualDisplay::othersOffAfter(LayoutStep::Impose, off, imposed);
        if (!off) setOthersOff(ctx, false);
        if (imposed)
            Logger::info(QStringLiteral("[vdisplay-apply] the other displays as they were, ours "
                                        "on their right — saved as this set's layout"));
        else
            Logger::warning(
                QStringLiteral("[vdisplay-apply] the layout from before not imposed: %1")
                    .arg(error));
        error.clear();
        if (auto again = findOurTarget()) target = again;
    }
    if (const auto saved = loadTopology(ctx); !stayExclusive && saved && !imposed) {
        const int pruned = pruneToTopology(*saved, target->devicePath,
                                           savesLayout(LayoutStep::Prune, off), &error);
        if (pruned < 0)
            Logger::warning(
                QStringLiteral("[vdisplay-apply] extra displays not switched off: %1").arg(error));
        else if (pruned > 0)
            Logger::info(QStringLiteral("[vdisplay-apply] %1 display(s) Windows switched on "
                                        "with ours switched off again")
                             .arg(pruned));
        if (auto again = findOurTarget()) target = again;
    }

    // The client's own size and its screen's rate when it asked for them, the
    // default mode when it did not — and the default mode again if that one
    // is refused, so a display that cannot take a phone's shape, or a rate
    // the driver did not list, still streams.
    int wantW = ctx.request.width, wantH = ctx.request.height;
    if (!VirtualDisplay::normaliseMode(wantW, wantH)) {
        wantW = VirtualDisplay::kWidth;
        wantH = VirtualDisplay::kHeight;
    }
    int wantMilli = ctx.request.refreshMilliHz;
    if (!VirtualDisplay::normaliseRate(wantMilli))
        wantMilli = VirtualDisplay::kRefreshHz * VirtualDisplay::kMilliHz;
    const bool saveMode = savesLayout(LayoutStep::Mode, off);
    if (setMode(target->devicePath, wantW, wantH, wantMilli, saveMode, &error)) {
        Logger::info(QStringLiteral("[vdisplay-apply] %1 set to %2x%3@%4")
                         .arg(target->gdiName)
                         .arg(wantW)
                         .arg(wantH)
                         .arg(VirtualDisplay::rateText(wantMilli)));
    } else {
        // Not fatal: the display exists and streams at whatever mode it has.
        Logger::warning(QStringLiteral("[vdisplay-apply] mode %1x%2@%3 refused on %4: %5")
                            .arg(wantW)
                            .arg(wantH)
                            .arg(VirtualDisplay::rateText(wantMilli))
                            .arg(target->gdiName, error));
        // The size first — a rate the client's screen runs at is worth more
        // than its shape — then the product's own rate (a rate matched to the
        // client's screen that the driver did not take), then the default
        // mode entirely.
        const struct
        {
            int w, h, milli;
        } fallbacks[] = {{VirtualDisplay::kWidth, VirtualDisplay::kHeight, wantMilli},
                         {VirtualDisplay::kWidth, VirtualDisplay::kHeight,
                          VirtualDisplay::kRateMax * VirtualDisplay::kMilliHz},
                         {VirtualDisplay::kWidth, VirtualDisplay::kHeight,
                          VirtualDisplay::kRefreshHz * VirtualDisplay::kMilliHz}};
        for (const auto& f : fallbacks) {
            if (f.w == wantW && f.h == wantH && f.milli == wantMilli) continue;
            QString again;
            if (!setMode(target->devicePath, f.w, f.h, f.milli, saveMode, &again)) continue;
            Logger::info(QStringLiteral("[vdisplay-apply] %1 fell back to %2x%3@%4")
                             .arg(target->gdiName)
                             .arg(f.w)
                             .arg(f.h)
                             .arg(VirtualDisplay::rateText(f.milli)));
            break;
        }
    }

    // SDR: the same call scripts/Set-DisplayHdr.ps1 makes. The target ids may
    // have moved with the mode change, so look the display up again. HDR
    // only for a bench that asked (Request::benchHdr, MW_BENCH_VD_HDR).
    if (auto again = findOurTarget()) target = again;
    const bool hdr = ctx.request.benchHdr;
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE ac{};
    ac.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    ac.header.size = sizeof(ac);
    ac.header.adapterId = target->adapterId;
    ac.header.id = target->targetId;
    ac.enableAdvancedColor = hdr ? 1 : 0;
    const LONG hr = DisplayConfigSetDeviceInfo(&ac.header);
    if (hr != ERROR_SUCCESS && hr != ERROR_NOT_SUPPORTED)
        Logger::warning(QStringLiteral("[vdisplay-apply] %1 refused: %2")
                            .arg(hdr ? QStringLiteral("HDR (bench)") : QStringLiteral("SDR"),
                                 lastErrorText(DWORD(hr))));
    else if (hdr)
        Logger::info(QStringLiteral("[vdisplay-apply] HDR left on for a bench (MW_BENCH_VD_HDR)"));

    if (ctx.request.benchSecondary)
        Logger::info(QStringLiteral("[vdisplay-apply] the primary display keeps its role, "
                                    "for a bench (MW_BENCH_VD_SECONDARY)"));
    else if (!makePrimary(target->devicePath, savesLayout(LayoutStep::Primary, off), &error))
        Logger::warning(QStringLiteral("[vdisplay-apply] not made primary: %1").arg(error));

    // Last, and never saved: every change above has been, with the other
    // displays on — the layout Windows replays whatever ends the stream.
    if (ctx.request.exclusive && !off) {
        // The marker first: a crash between the two leaves a marker with the
        // displays on, which only spares a save.
        setOthersOff(ctx, true);
        const int n = keepOnlyOurs(target->devicePath, &error);
        off = VirtualDisplay::othersOffAfter(LayoutStep::Exclusive, off, n >= 0);
        if (n >= 0)
            Logger::info(
                QStringLiteral("[vdisplay-apply] exclusive: %1 other display(s) off").arg(n));
        else
            Logger::warning(
                QStringLiteral("[vdisplay-apply] exclusive: the others stay on: %1").arg(error));
        if (!off) setOthersOff(ctx, false);
    } else if (off) {
        // The layout from before could not be imposed again: the others stay
        // off until the stream ends, whatever the option says now.
        Logger::info(QStringLiteral("[vdisplay-apply] the other displays stay off until the "
                                    "stream ends"));
    }

    ctx.result.ok = true;
    ctx.result.stage = QStringLiteral("done");
    return finish(ctx);
}

#endif // Q_OS_WIN

} // namespace

int run(const QString& stageArg, const QString& dirArg)
{
    Context ctx;
    ctx.dir = dirArg.isEmpty() ? VirtualDisplay::stagingDir() : QDir::fromNativeSeparators(dirArg);
    const QString stage = stageArg.isEmpty() ? QStringLiteral("all") : stageArg;

    QFile req(ctx.dir + QStringLiteral("/request.json"));
    if (!req.open(QIODevice::ReadOnly))
        return failAt(ctx, QStringLiteral("request"),
                      QStringLiteral("no request file at %1").arg(req.fileName()));
    QString error;
    const auto parsed = VirtualDisplay::parseRequest(req.readAll(), &error);
    if (!parsed) return failAt(ctx, QStringLiteral("request"), error);
    ctx.request = *parsed;

#ifdef Q_OS_WIN
    using Action = VirtualDisplay::Request::Action;
    const bool all = stage == QLatin1String("all");
    const bool wantSnapshot = all || stage == QLatin1String("snapshot");
    const bool wantDriver = all || stage == QLatin1String("driver");
    const bool wantMode = all || stage == QLatin1String("mode");
    if (!wantSnapshot && !wantDriver && !wantMode)
        return failAt(ctx, QStringLiteral("request"),
                      QStringLiteral("unknown stage %1").arg(stage));

    // Activate: remember the desktop, switch the node on, set the desktop.
    // Deactivate: the desktop half first (the layout back while our display
    // still exists), then the node. Every other verb: the node, then the
    // desktop half, which has nothing to do.
    std::vector<int (*)(Context&)> stages;
    if (ctx.request.action == Action::Activate) {
        if (wantSnapshot) stages.push_back(&stageSnapshot);
        if (wantDriver) stages.push_back(&stageDriver);
        if (wantMode) stages.push_back(&stageMode);
    } else if (ctx.request.action == Action::Deactivate) {
        if (wantMode) stages.push_back(&stageMode);
        if (wantDriver) stages.push_back(&stageDriver);
    } else if (ctx.request.action == Action::OthersBack) {
        if (wantMode) stages.push_back(&stageMode);
    } else {
        if (wantDriver) stages.push_back(&stageDriver);
        if (wantMode) stages.push_back(&stageMode);
    }
    if (stages.empty()) {
        ctx.result.ok = true;
        ctx.result.stage = QStringLiteral("done");
        return finish(ctx);
    }

    for (const auto stageFn : stages) {
        const int rc = stageFn(ctx);
        // A stage finishes itself on failure and when the verb is complete;
        // otherwise it returns without writing so the next one can go on.
        if (!ctx.result.ok || ctx.result.stage == QLatin1String("done")) return rc;
    }
    // Ends on a stage that does not finish itself (Deactivate's driver
    // stage, or one half of a split run): the caller reads the result file.
    if (all) ctx.result.stage = QStringLiteral("done");
    return finish(ctx);
#else
    Q_UNUSED(stage);
    return failAt(ctx, QStringLiteral("request"),
                  QStringLiteral("virtual displays are not supported on this platform"));
#endif
}

int guard(const QString& server)
{
#ifdef Q_OS_WIN
    const QStringList parts = server.split(QLatin1Char(':'));
    bool okPid = false, okTime = false;
    const DWORD pid = parts.size() == 2 ? parts[0].toULong(&okPid) : 0;
    const quint64 created = parts.size() == 2 ? parts[1].toULongLong(&okTime) : 0;
    if (!okPid || !okTime || pid == 0) {
        Logger::warning(QStringLiteral("[vdisplay-guard] not a server: %1").arg(server));
        return kExitFailed;
    }
    // One guard per edition and session: an exclusive stream that takes a
    // new mode activates again, and the first guard is still on watch.
    const std::wstring mutexName =
        (QStringLiteral("Local\\") + VirtualDisplay::displayName() + QStringLiteral(" guard"))
            .toStdWString();
    HANDLE single = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single);
        return kExitOk;
    }
    // The handle, not the number, and only when it is the server's: a pid
    // the server left behind may name another process by now.
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        FILETIME c{}, e{}, k{}, u{};
        const quint64 have = GetProcessTimes(proc, &c, &e, &k, &u)
                                 ? (quint64(c.dwHighDateTime) << 32) | quint64(c.dwLowDateTime)
                                 : 0;
        if (have != created) {
            CloseHandle(proc);
            proc = nullptr;
        }
    }
    Context ctx;
    ctx.dir = VirtualDisplay::stagingDir();
    // Off means a marker that is there AND can be read: anything less and
    // nothing is touched.
    const auto othersOffReadable = [&ctx] {
        QFile f(othersOffPath(ctx));
        return f.exists() && f.open(QIODevice::ReadOnly);
    };
    Logger::info(QStringLiteral("[vdisplay-guard] watching the server (pid %1)%2")
                     .arg(pid)
                     .arg(proc ? QString() : QStringLiteral(" — already gone")));
    int rc = kExitOk;
    for (;;) {
        const bool alive = proc && WaitForSingleObject(proc, 1000) == WAIT_TIMEOUT;
        const VirtualDisplay::GuardStep step =
            VirtualDisplay::guardStep(alive, othersOffReadable());
        if (step == VirtualDisplay::GuardStep::Wait) continue;
        if (step == VirtualDisplay::GuardStep::Restore) {
            Logger::warning(QStringLiteral("[vdisplay-guard] the server is gone with the other "
                                           "displays off — bringing them back"));
            VirtualDisplay::Request req;
            req.action = VirtualDisplay::Request::Action::Deactivate;
            QFile f(VirtualDisplay::requestPath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(VirtualDisplay::toJson(req));
                f.close();
                rc = run(QStringLiteral("mode"), QString());
            } else {
                Logger::warning(
                    QStringLiteral("[vdisplay-guard] cannot write %1").arg(f.fileName()));
                rc = kExitFailed;
            }
        }
        break;
    }
    if (proc) CloseHandle(proc);
    if (single) CloseHandle(single);
    return rc;
#else
    Q_UNUSED(server);
    return kExitOk;
#endif
}

} // namespace VirtualDisplayApply
