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

#include "backend/VirtualDisplayJob.h"
#include "backend/streambackend/NativeProbeService.h"
#include "common/Edition.h"
#include "common/Logger.h"
#include "common/WinSystemPath.h"
#include "mw/native/Capabilities.h"
#include "mw/native/VirtualDisplay.h"
#include "server/AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <comdef.h>
#include <taskschd.h>
#include <vector>
#endif

namespace VirtualDisplay {

// ── Paths ───────────────────────────────────────────────────────────────────

QString stagingDir()
{
#ifdef Q_OS_WIN
    // Per-user and under the edition's own name, like SelfUpdater and
    // GamepadDriver: an elevated task registered for this user runs a fixed
    // command, and a DEV install must never stage into production's.
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (!base.isEmpty() && !mw::win::isUnderSystem32(base))
        return base + QLatin1Char('/') + mw::edition::productName() + QStringLiteral("/vdisplay");
    // A service has SYSTEM's profile, under System32. The install directory is
    // admin-only, which is the property that matters.
    return QCoreApplication::applicationDirPath() + QStringLiteral("/vdisplay");
#else
    return QDir::tempPath() + QStringLiteral("/MoonlightWeb-vdisplay");
#endif
}

QString requestPath()
{
    return stagingDir() + QStringLiteral("/request.json");
}

QString resultPath()
{
    return stagingDir() + QStringLiteral("/result.json");
}

QString othersOffPath()
{
    return stagingDir() + QStringLiteral("/others-off");
}

QString bundledDriverDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/drivers/vdd");
}

// ── Status ──────────────────────────────────────────────────────────────────

QJsonObject toJson(const Status& st)
{
    QJsonObject obj;
    obj["supported"] = st.supported;
    obj["installed"] = st.installed;
    obj["enabled"] = st.enabled;
    obj["active"] = st.active;
    obj["can_manage"] = st.canManage;
    obj["method"] = st.method;
    obj["name"] = displayName();
    return obj;
}

namespace {

bool anyOurs(const mw::native::Capabilities& caps)
{
    for (const mw::native::DisplayInfo& d : caps.displays)
        if (isOurs(d)) return true;
    return false;
}

} // namespace

#ifdef Q_OS_WIN

namespace {

struct ComScope
{
    HRESULT hr;
    ComScope()
        : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {}
    ~ComScope()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Bstr
{
    BSTR b;
    explicit Bstr(const QString& s)
        : b(SysAllocString(reinterpret_cast<const OLECHAR*>(s.utf16())))
    {}
    ~Bstr() { SysFreeString(b); }
};

QString bstrToQString(BSTR b)
{
    return b ? QString::fromWCharArray(b, static_cast<int>(SysStringLen(b))) : QString();
}

/// Strip surrounding quotes and normalise separators for a path comparison.
QString canonicalExe(QString s)
{
    s = s.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        s = s.mid(1, s.size() - 2);
    return QDir::cleanPath(QDir::fromNativeSeparators(s)).toLower();
}

/// The installer's task exists AND its action is this very exe with the fixed
/// argument. Both checked: a task pointing elsewhere (an install moved, an
/// older layout) must not be reported as a way to elevate.
bool elevatedTaskUsable()
{
    ComScope com;
    if (!com.usable()) return false;
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.out()));
    if (FAILED(hr)) return false;
    VARIANT none;
    VariantInit(&none);
    if (FAILED(service->Connect(none, none, none, none))) return false;
    ComPtr<ITaskFolder> root;
    Bstr rootPath(QStringLiteral("\\"));
    if (FAILED(service->GetFolder(rootPath.b, root.out()))) return false;

    Bstr name(taskName());
    ComPtr<IRegisteredTask> task;
    if (FAILED(root->GetTask(name.b, task.out())) || !task) return false;

    ComPtr<ITaskDefinition> def;
    if (FAILED(task->get_Definition(def.out())) || !def) return false;
    ComPtr<IActionCollection> actions;
    if (FAILED(def->get_Actions(actions.out())) || !actions) return false;
    ComPtr<IAction> action;
    if (FAILED(actions->get_Item(1, action.out())) || !action) return false;
    ComPtr<IExecAction> exec;
    if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.out()))) ||
        !exec)
        return false;

    BSTR path = nullptr;
    BSTR args = nullptr;
    exec->get_Path(&path);
    exec->get_Arguments(&args);
    const QString taskExe = canonicalExe(bstrToQString(path));
    const QString taskArgs = bstrToQString(args).trimmed();
    SysFreeString(path);
    SysFreeString(args);

    const QString ourExe = canonicalExe(QCoreApplication::applicationFilePath());
    if (taskExe != ourExe) {
        Logger::info(QStringLiteral("[vdisplay] task \"%1\" runs %2, not this exe — unusable")
                         .arg(taskName(), taskExe));
        return false;
    }
    return taskArgs == applyArgument();
}

/// Cached: the answer changes only with an install, and each call is a COM
/// round trip the hosts page would otherwise pay on every poll.
QString installMethod()
{
    static QString cached;
    static QElapsedTimer age;
    if (age.isValid() && age.elapsed() < 30 * 1000) return cached;

    if (processElevated())
        cached = QStringLiteral("elevated");
    else if (elevatedTaskUsable())
        cached = QStringLiteral("task");
    else
        cached.clear();
    age.restart();
    return cached;
}

/// A registry property of a devnode, as a string (REG_SZ; the first string
/// of a REG_MULTI_SZ). Empty when absent.
QString devnodeProperty(DEVINST inst, ULONG property)
{
    ULONG size = 0;
    if (CM_Get_DevNode_Registry_PropertyW(inst, property, nullptr, nullptr, &size, 0) !=
            CR_BUFFER_SMALL ||
        size == 0)
        return QString();
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (CM_Get_DevNode_Registry_PropertyW(inst, property, nullptr, buf.data(), &size, 0) !=
        CR_SUCCESS)
        return QString();
    return QString::fromWCharArray(buf.data());
}

bool hasHardwareId(DEVINST inst, const QString& wanted)
{
    ULONG size = 0;
    if (CM_Get_DevNode_Registry_PropertyW(inst, CM_DRP_HARDWAREID, nullptr, nullptr, &size, 0) !=
            CR_BUFFER_SMALL ||
        size == 0)
        return false;
    std::vector<wchar_t> ids(size / sizeof(wchar_t) + 1, 0);
    if (CM_Get_DevNode_Registry_PropertyW(inst, CM_DRP_HARDWAREID, nullptr, ids.data(), &size, 0) !=
        CR_SUCCESS)
        return false;
    for (const wchar_t* h = ids.data(); *h; h += wcslen(h) + 1)
        if (QString::fromWCharArray(h).compare(wanted, Qt::CaseInsensitive) == 0) return true;
    return false;
}

/// Our device node, asked of the configuration manager: a root-enumerated
/// node with the driver's hardware id AND our friendly name. The owner's own
/// VDD has the first and not the second. Disabled nodes count (the normal
/// state of ours); a driver package left in the store without a node (after
/// an uninstall) does not.
std::optional<DEVINST> ourNode()
{
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"ROOT", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS ||
        len == 0)
        return std::nullopt;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(L"ROOT", buf.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR) !=
        CR_SUCCESS)
        return std::nullopt;

    const QString wantedName = displayName();
    for (const wchar_t* id = buf.data(); *id; id += wcslen(id) + 1) {
        DEVINST inst = 0;
        // Phantom devnodes too: a disabled node is still present, but the
        // flag costs nothing and a node Windows has not started yet is ours
        // just the same.
        if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(id), CM_LOCATE_DEVNODE_PHANTOM) !=
            CR_SUCCESS)
            continue;
        if (!hasHardwareId(inst, hardwareId())) continue;
        if (devnodeProperty(inst, CM_DRP_FRIENDLYNAME).compare(wantedName, Qt::CaseInsensitive) ==
            0)
            return inst;
    }
    return std::nullopt;
}

/// The monitor device ids hanging off our node ("DISPLAY\MTT1337\1&15ecd195&0&
/// UID256"), spelled the way a monitor device PATH spells them: '\' → '#',
/// lower case — so a probe key ("\\?\DISPLAY#MTT1337#1&15ecd195&0&UID256#{…}")
/// can be matched by substring. Empty while the node is disabled: a stopped
/// driver has no children.
QStringList ourMonitorIds()
{
    QStringList out;
    const auto node = ourNode();
    if (!node) return out;
    DEVINST child = 0;
    if (CM_Get_Child(&child, *node, 0) != CR_SUCCESS) return out;
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

} // namespace

bool nodePresent()
{
    return ourNode().has_value();
}

bool nodeEnabled()
{
    const auto node = ourNode();
    if (!node) return false;
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, *node, 0) != CR_SUCCESS) return false;
    return !((status & DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED);
}

bool isOurs(const mw::native::DisplayInfo& display)
{
    if (display.kind != mw::native::DisplayKind::Virtual) return false;
    const QString key = QString::fromStdString(display.key).toLower();
    for (const QString& id : ourMonitorIds())
        if (key.contains(id)) return true;
    return false;
}

bool processElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

bool elevatedTaskRunning()
{
    ComScope com;
    if (!com.usable()) return false;
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.out()));
    if (FAILED(hr)) return false;
    VARIANT none;
    VariantInit(&none);
    if (FAILED(service->Connect(none, none, none, none))) return false;
    ComPtr<ITaskFolder> root;
    Bstr rootPath(QStringLiteral("\\"));
    if (FAILED(service->GetFolder(rootPath.b, root.out()))) return false;
    Bstr name(taskName());
    ComPtr<IRegisteredTask> task;
    if (FAILED(root->GetTask(name.b, task.out())) || !task) return false;
    TASK_STATE state = TASK_STATE_UNKNOWN;
    if (FAILED(task->get_State(&state))) return false;
    return state == TASK_STATE_RUNNING;
}

Status probe()
{
    Status st;
#if defined(Q_PROCESSOR_ARM)
    // The native engine runs on Windows ARM64, but Windows on ARM takes a
    // driver package only signed by Microsoft (0x800b0109, 20/10/2026): none
    // is shipped there, so there is nothing to offer.
    return st;
#else
    st.supported = true;
    st.installed = nodePresent();
    st.enabled = st.installed && nodeEnabled();
    st.method = installMethod();
    st.canManage = !st.method.isEmpty();
    st.active = st.enabled && anyOurs(NativeProbeService::instance().snapshot());
    return st;
#endif
}

bool applyInProcess(const Request& req, Result* result)
{
    Q_UNUSED(req);
    Result res;
    res.stage = QStringLiteral("driver");
    res.error = QStringLiteral("the virtual display is a driver on Windows, not an object");
    if (result) *result = res;
    return false;
}

void resetAtStartup()
{
    // Killed mid-stream, the previous process left the node enabled and the
    // desktop on our display. Put both back — through the same elevated path
    // a stream's end takes, with the primary the record remembers.
    if (!nodePresent() || !nodeEnabled()) {
        AppSettings().clearVirtualDisplay();
        return;
    }
    Logger::info(QStringLiteral("[vdisplay] the virtual display was left on — turning it off"));
    VirtualDisplayJob::instance().deactivate(nullptr);
}

#elif defined(Q_OS_LINUX) // the portal's VIRTUAL source, made by the stream itself

bool nodePresent()
{
    // The probe lists it whenever the portal offers the source type.
    return anyOurs(NativeProbeService::instance().snapshot());
}

bool nodeEnabled()
{
    return false;
}

bool processElevated()
{
    return false;
}

bool elevatedTaskRunning()
{
    return false;
}

bool isOurs(const mw::native::DisplayInfo& display)
{
    return display.key == mw::native::kPortalVirtualDisplayKey;
}

Status probe()
{
    Status st;
    st.supported = st.installed = nodePresent();
    st.method = st.supported ? QStringLiteral("portal") : QString();
    return st;
}

bool applyInProcess(const Request& req, Result* result)
{
    // Never reached from /start (livesInStream): nothing to turn on or off.
    Q_UNUSED(req);
    Result res;
    res.ok = true;
    res.stage = QStringLiteral("done");
    if (result) *result = res;
    return true;
}

void resetAtStartup()
{
    AppSettings().clearVirtualDisplay();
}

#else // macOS: the engine's own display (mw::native::vdisplay)

bool nodePresent()
{
    return mw::native::vdisplay::isSupported();
}

bool nodeEnabled()
{
    return mw::native::vdisplay::isActive();
}

bool processElevated()
{
    return false;
}

bool elevatedTaskRunning()
{
    return false;
}

bool isOurs(const mw::native::DisplayInfo& display)
{
    // The display the server process created carries the name it was
    // created with, and AppKit hands that name back as the screen's own.
    // By name only: the stream worker is another process, which holds no
    // display object of its own but must recognise the server's.
    if (display.kind != mw::native::DisplayKind::Virtual) return false;
    return QString::fromStdString(display.model).compare(displayName(), Qt::CaseInsensitive) == 0;
}

Status probe()
{
    Status st;
    st.supported = mw::native::vdisplay::isSupported();
    if (!st.supported) return st;
    st.installed = true;
    st.enabled = mw::native::vdisplay::isActive();
    st.method = QStringLiteral("inprocess");
    st.canManage = true;
    st.active = st.enabled && anyOurs(NativeProbeService::instance().snapshot());
    return st;
}

bool applyInProcess(const Request& req, Result* result)
{
    Result res;
    res.stage = QStringLiteral("mode");
    switch (req.action) {
    case Request::Action::Install:
    case Request::Action::Uninstall:
    case Request::Action::OthersBack:
        // Nothing to install: the display is conjured at activation. Nor are
        // the other displays ever off here (Windows only).
        res.ok = true;
        res.stage = QStringLiteral("done");
        break;
    case Request::Action::Deactivate: {
        // The previous main display first, while ours still exists — the OS
        // would otherwise pick one itself — then the release.
        bool okRestore = true;
        const uint32_t previous = req.restorePrimary.toUInt(&okRestore);
        if (okRestore && previous != 0 && mw::native::vdisplay::isActive()) {
            std::string error;
            if (!mw::native::vdisplay::setMain(previous, &error))
                Logger::warning(QStringLiteral("[vdisplay] could not restore the main display: %1")
                                    .arg(QString::fromStdString(error)));
        }
        mw::native::vdisplay::destroy();
        res.ok = true;
        res.stage = QStringLiteral("done");
        break;
    }
    case Request::Action::Activate: {
        if (mw::native::vdisplay::isActive()) {
            res.ok = true;
            res.display = QStringLiteral("display %1").arg(mw::native::vdisplay::displayId());
            break;
        }
        // CoreGraphics takes any size and any rate the client asks for, no
        // driver and no mode list in between: "Match my screen" on macOS is
        // simply a display created at that size, at the client's own cadence.
        mw::native::vdisplay::Spec spec;
        int w = req.width, h = req.height;
        if (!normaliseMode(w, h)) {
            w = kWidth;
            h = kHeight;
        }
        // CoreGraphics takes whole hertz; the stream's rate is one on macOS.
        int milli = req.refreshMilliHz;
        const int hz = normaliseRate(milli) ? (milli + kMilliHz / 2) / kMilliHz : kRefreshHz;
        spec.width = w;
        spec.height = h;
        spec.refreshHz = hz;
        spec.hdr = req.hdr;
        spec.name = displayName().toStdString();
        res.previousPrimary = QString::number(mw::native::vdisplay::mainDisplay());
        std::string error;
        res.ok = mw::native::vdisplay::create(spec, &error);
        if (res.ok)
            res.display = QStringLiteral("display %1").arg(mw::native::vdisplay::displayId());
        else
            res.error = QString::fromStdString(error);
        break;
    }
    }
    if (result) *result = res;
    return res.ok;
}

void resetAtStartup()
{
    // The display died with the previous process; only its record is left.
    AppSettings().clearVirtualDisplay();
}

#endif

bool livesInStream()
{
#if defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

} // namespace VirtualDisplay
