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

#include "StreamPriority.h"

#include "../../core/Log.h"

#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// Newer than some SDKs this builds against (22000+ for the timer flag).
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace mw::native {

namespace {

std::string hex(long value)
{
    char out[16];
    std::snprintf(out, sizeof(out), "0x%08lX", static_cast<unsigned long>(value));
    return out;
}

bool envIs(const char* name, const char* value)
{
    char buf[32] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && _stricmp(buf, value) == 0;
}

void leaveEcoQos()
{
    if (envIs("MW_ECOQOS", "keep")) {
        log::info("[native] priority: MW_ECOQOS=keep — Windows may still throttle this process");
        return;
    }
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // In the mask with a zero state: "never throttle this", as opposed to
    // leaving the decision to the OS's heuristics.
    state.ControlMask =
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    if (::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &state,
                                sizeof(state)))
        log::info("[native] priority: out of EcoQoS (full-speed cores, timer requests honoured)");
    else
        log::info("[native] priority: EcoQoS opt-out refused (error " +
                  std::to_string(::GetLastError()) + ")");
}

// d3dkmthk.h is a WDK header; the few entry points needed are exported by
// gdi32, so they are looked up rather than linked, and the structures they
// take are restated here as the header lays them out.
enum KmtSchedulingClass : int
{
    KmtIdle,
    KmtBelowNormal,
    KmtNormal,
    KmtAboveNormal,
    KmtHigh,
    KmtRealtime
};
using SetSchedulingClassFn = LONG(APIENTRY*)(HANDLE, int);

struct KmtOpenAdapterFromLuid
{
    LUID luid;
    UINT adapter;
};
struct KmtQueryAdapterInfo
{
    UINT adapter;
    int type;
    void* data;
    UINT size;
};
struct KmtCloseAdapter
{
    UINT adapter;
};
constexpr int KmtQaiWddm27Caps = 70; // KMTQAITYPE_WDDM_2_7_CAPS
using OpenAdapterFromLuidFn = LONG(APIENTRY*)(KmtOpenAdapterFromLuid*);
using QueryAdapterInfoFn = LONG(APIENTRY*)(const KmtQueryAdapterInfo*);
using CloseAdapterFn = LONG(APIENTRY*)(const KmtCloseAdapter*);

HMODULE gdi32()
{
    HMODULE gdi = ::GetModuleHandleW(L"gdi32.dll");
    return gdi ? gdi : ::LoadLibraryW(L"gdi32.dll");
}

/// Who this process runs as, in the terms the REALTIME class cares about: it
/// takes SeIncreaseBasePriorityPrivilege, which SYSTEM and an elevated
/// administrator hold and a filtered token does not.
std::string tokenKind()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return "unknown";
    std::string kind = "limited";
    BYTE user[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)] = {};
    DWORD len = 0;
    if (::GetTokenInformation(token, TokenUser, user, sizeof(user), &len) &&
        ::IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid)) {
        kind = "SYSTEM";
    } else {
        TOKEN_ELEVATION elevation = {};
        if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &len) &&
            elevation.TokenIsElevated)
            kind = "elevated";
    }
    ::CloseHandle(token);
    return kind;
}

/// Turns SeIncreaseBasePriorityPrivilege on for this process. Held is not
/// enabled: an elevated token carries it switched off.
bool enableBasePriorityPrivilege()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = ::LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege",
                                            &tp.Privileges[0].Luid) &&
                    ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
                    ::GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    ::CloseHandle(token);
    return ok;
}

std::atomic<StreamPriority::GpuClass> g_GrantedClass{StreamPriority::GpuClass::Unknown};
std::atomic<StreamPriority::Role> g_Role{StreamPriority::Role::Owner};

/// The guests' feed: one class below what an owner's session gets with the
/// same token (see the header). HIGH and ABOVE_NORMAL ask nothing of the token.
void lowerFeedGpuScheduling(SetSchedulingClassFn set, const std::string& who)
{
    const bool ownerRealtime = who != "limited" && !envIs("MW_GPU_PRIORITY", "high");
    const StreamPriority::GpuClass want = StreamPriority::feedClassToAsk(ownerRealtime);
    const bool high = want == StreamPriority::GpuClass::High;
    const LONG first = set(::GetCurrentProcess(), high ? KmtHigh : KmtAboveNormal);
    if (first == 0) {
        g_GrantedClass = want;
        log::info(std::string("[native] priority: guests' feed, GPU scheduling class ") +
                  StreamPriority::toString(want) + ", one below the owner's (token " + who + ")");
        return;
    }
    const LONG lower = high ? set(::GetCurrentProcess(), KmtAboveNormal) : first;
    g_GrantedClass = high && lower == 0 ? StreamPriority::GpuClass::AboveNormal
                                        : StreamPriority::GpuClass::Normal;
    log::info(std::string("[native] priority: guests' feed, GPU scheduling class ") +
              StreamPriority::toString(want) + " refused (" + hex(first) + "), " +
              StreamPriority::toString(g_GrantedClass.load()) + " instead");
}

void raiseGpuScheduling()
{
    const auto set = reinterpret_cast<SetSchedulingClassFn>(
        ::GetProcAddress(gdi32(), "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!set) {
        g_GrantedClass = StreamPriority::GpuClass::Normal;
        log::info("[native] priority: no GPU scheduling class on this Windows");
        return;
    }
    // REALTIME puts this process's GPU work in front of the game's, which
    // under a real game takes the encode p99 from 10-16 ms to 3 ms with no
    // desktop stall seen (docs/bench-native-host.md §8l). Only a privileged
    // token can have it; MW_GPU_PRIORITY=high keeps the older class, for a
    // bench's before.
    const std::string who = tokenKind();
    if (g_Role.load() == StreamPriority::Role::Feed) {
        lowerFeedGpuScheduling(set, who);
        return;
    }
    if (envIs("MW_GPU_PRIORITY", "high")) {
        log::info("[native] priority: MW_GPU_PRIORITY=high — REALTIME not asked");
    } else if (who == "limited") {
        log::info("[native] priority: GPU scheduling class REALTIME needs the SYSTEM or elevated "
                  "worker (token limited)");
    } else {
        const bool privilege = enableBasePriorityPrivilege();
        const LONG realtime = set(::GetCurrentProcess(), KmtRealtime);
        if (realtime == 0) {
            g_GrantedClass = StreamPriority::GpuClass::Realtime;
            log::info("[native] priority: GPU scheduling class REALTIME (token " + who + ")");
            return;
        }
        log::info("[native] priority: GPU scheduling class REALTIME refused (" + hex(realtime) +
                  ", token " + who + (privilege ? "" : ", no base-priority privilege") +
                  "), asking HIGH");
    }
    const LONG high = set(::GetCurrentProcess(), KmtHigh);
    if (high == 0) {
        g_GrantedClass = StreamPriority::GpuClass::High;
        log::info("[native] priority: GPU scheduling class HIGH");
        return;
    }
    const LONG above = set(::GetCurrentProcess(), KmtAboveNormal);
    g_GrantedClass =
        above == 0 ? StreamPriority::GpuClass::AboveNormal : StreamPriority::GpuClass::Normal;
    log::info("[native] priority: GPU scheduling class HIGH refused (" + hex(high) + ")" +
              (above == 0 ? ", ABOVE_NORMAL instead" : ", left NORMAL"));
}

std::atomic<bool> g_ProcessDone{false};

} // namespace

void StreamPriority::engageProcess()
{
    if (g_ProcessDone.exchange(true)) return;
    leaveEcoQos();
    if (!envIs("MW_GPU_PRIORITY", "normal")) {
        raiseGpuScheduling();
    } else {
        g_GrantedClass = GpuClass::LeftAlone;
        log::info("[native] priority: MW_GPU_PRIORITY=normal — GPU scheduling left as is");
    }
    // The guests' feed waits for the owner on the CPU too: its capture,
    // conversion and encode run below every ordinary thread of the machine.
    if (g_Role.load() == Role::Feed) {
        if (::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS))
            log::info("[native] priority: guests' feed, CPU priority class BELOW_NORMAL");
        else
            log::info("[native] priority: guests' feed, CPU priority class BELOW_NORMAL "
                      "refused (error " +
                      std::to_string(::GetLastError()) + ")");
        return;
    }
    // Bench switch, off by default: the whole process one class up on the
    // CPU. Never REALTIME, which can starve the input stack itself.
    if (envIs("MW_CPU_PRIORITY", "high")) {
        if (::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS))
            log::info("[native] priority: CPU priority class HIGH");
        else
            log::info("[native] priority: CPU priority class HIGH refused (error " +
                      std::to_string(::GetLastError()) + ")");
    }
}

void StreamPriority::engage()
{
    engageProcess();

    if (m_PowerRequest) return;
    REASON_CONTEXT reason = {};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString =
        const_cast<LPWSTR>(L"MoonlightWeb is streaming this display");
    m_PowerRequest = ::PowerCreateRequest(&reason);
    if (m_PowerRequest == INVALID_HANDLE_VALUE) m_PowerRequest = nullptr;
    if (!m_PowerRequest || !::PowerSetRequest(m_PowerRequest, PowerRequestSystemRequired) ||
        !::PowerSetRequest(m_PowerRequest, PowerRequestDisplayRequired)) {
        log::info("[native] priority: could not keep the machine awake (error " +
                  std::to_string(::GetLastError()) + ")");
    }
}

StreamPriority::GpuClass StreamPriority::grantedClass()
{
    return g_GrantedClass.load();
}

const char* StreamPriority::toString(GpuClass gpuClass)
{
    switch (gpuClass) {
    case GpuClass::Unknown: return "unknown";
    case GpuClass::LeftAlone: return "left alone";
    case GpuClass::Normal: return "NORMAL";
    case GpuClass::AboveNormal: return "ABOVE_NORMAL";
    case GpuClass::High: return "HIGH";
    case GpuClass::Realtime: return "REALTIME";
    }
    return "unknown";
}

D3D12_COMMAND_QUEUE_PRIORITY StreamPriority::queuePriority()
{
    return queuePriorityFor(role(), grantedClass());
}

D3D12_COMMAND_QUEUE_PRIORITY StreamPriority::queuePriorityFor(Role role, GpuClass gpuClass)
{
    // The feed's queues are raised only as far as its class: HIGH under HIGH
    // (where the owner's are GLOBAL_REALTIME), NORMAL otherwise.
    if (role == Role::Feed)
        return gpuClass == GpuClass::High ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH
                                          : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    switch (gpuClass) {
    case GpuClass::Realtime: return D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    case GpuClass::LeftAlone: return D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    default: break;
    }
    return D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

StreamPriority::GpuClass StreamPriority::feedClassToAsk(bool ownerGetsRealtime)
{
    return ownerGetsRealtime ? GpuClass::High : GpuClass::AboveNormal;
}

int StreamPriority::deviceThreadPriorityFor(Role role)
{
    return role == Role::Feed ? 0 : 7;
}

void StreamPriority::setRole(Role role)
{
    if (role == Role::Feed && envIs("MW_FEED_PRIORITY", "same")) {
        log::info("[native] priority: MW_FEED_PRIORITY=same — the guests' feed keeps the "
                  "owner's standing");
        role = Role::Owner;
    } else if (role == Role::Feed && g_ProcessDone.load()) {
        // Too late to lower what engageProcess() already raised: said, not hidden.
        log::info("[native] priority: the guests' feed was named after the process was raised — "
                  "left at the owner's standing");
        return;
    }
    g_Role = role;
}

StreamPriority::Role StreamPriority::role()
{
    return g_Role.load();
}

bool StreamPriority::takesMmcss()
{
    return role() != Role::Feed;
}

void StreamPriority::release()
{
    if (!m_PowerRequest) return;
    ::PowerClearRequest(m_PowerRequest, PowerRequestDisplayRequired);
    ::PowerClearRequest(m_PowerRequest, PowerRequestSystemRequired);
    ::CloseHandle(m_PowerRequest);
    m_PowerRequest = nullptr;
}

namespace {

/// Whether the GPU schedules itself (hardware-accelerated GPU scheduling):
/// under it the classes above are carried out by the GPU's own firmware, so a
/// priority measured with it on says little about it off.
void logHardwareScheduling(IDXGIDevice* dxgi, const char* role)
{
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return;
    const HMODULE gdi = gdi32();
    const auto open =
        reinterpret_cast<OpenAdapterFromLuidFn>(::GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    const auto query =
        reinterpret_cast<QueryAdapterInfoFn>(::GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    const auto close =
        reinterpret_cast<CloseAdapterFn>(::GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return;
    KmtOpenAdapterFromLuid opened = {desc.AdapterLuid, 0};
    if (open(&opened) != 0) return;
    UINT caps = 0;
    const KmtQueryAdapterInfo info = {opened.adapter, KmtQaiWddm27Caps, &caps, sizeof(caps)};
    const LONG status = query(&info);
    const KmtCloseAdapter closing = {opened.adapter};
    close(&closing);
    std::string state;
    if (status != 0)
        state = "unknown (" + hex(status) + ")";
    else if (!(caps & 0x1))
        state = "not supported";
    else
        state = (caps & 0x2) ? "on" : "off";
    log::info(std::string("[native] priority: hardware GPU scheduling (HAGS) ") + state +
              ", for the " + role + " device's GPU");
}

} // namespace

void StreamPriority::raiseDevice(ID3D11Device* device, const char* role)
{
    if (!device) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) return;
    logHardwareScheduling(dxgi.Get(), role);
    if (envIs("MW_GPU_PRIORITY", "normal")) return;
    const int priority = deviceThreadPriorityFor(StreamPriority::role());
    if (priority == 0) {
        log::info(std::string("[native] priority: guests' feed, GPU thread priority left at "
                              "Windows' default on the ") +
                  role + " device");
        return;
    }
    const HRESULT hr = dxgi->SetGPUThreadPriority(priority);
    if (SUCCEEDED(hr))
        log::info(std::string("[native] priority: GPU thread priority ") +
                  std::to_string(priority) + " on the " + role + " device");
    else
        log::info(std::string("[native] priority: GPU thread priority refused on the ") + role +
                  " device (" + hex(hr) + ")");
}

} // namespace mw::native
