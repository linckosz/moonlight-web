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

#include "AudioSources.h"

#include "HostAppImage.h"

#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <iterator>
#include <thread>
#include <vector>
#endif

namespace AudioSources {

namespace {

struct Entry
{
    QString exe;  // lower case file name
    QString path; // full path, for the icon and the description
    QString name;
    bool playing = false;
};

/// Icons and names already read, by full path: an executable does not change
/// its icon between two visits to the settings page.
struct Cached
{
    QString name;
    QString icon;
};
QMutex g_CacheMutex;
QHash<QString, Cached> g_Cache;
/// The last icon seen for each executable name, so an excluded app that is not
/// running still shows its picture after it quits.
QHash<QString, Cached> g_ByExe;

QString pngDataUrl(const QImage& image)
{
    if (image.isNull()) return {};
    QImage scaled = image.width() > 48 || image.height() > 48
                        ? image.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                        : image;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!scaled.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

/// "chrome.exe" → "Chrome": what to call an app whose file says nothing better.
QString nameFromExe(const QString& exe)
{
    QString base = exe;
    if (base.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) base.chop(4);
    if (!base.isEmpty()) base[0] = base[0].toUpper();
    return base;
}

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

QString processPath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buf));
    QString path;
    if (QueryFullProcessImageNameW(h, 0, buf, &size))
        path = QString::fromWCharArray(buf, static_cast<int>(size));
    CloseHandle(h);
    return path;
}

/// The file's own description ("Google Chrome", "Discord"), as Explorer and the
/// volume mixer show it.
QString fileDescription(const QString& path)
{
    const std::wstring wpath = path.toStdWString();
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(wpath.c_str(), 0, size, data.data())) return {};
    struct Translation
    {
        WORD language;
        WORD codePage;
    }* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &bytes) ||
        bytes < sizeof(Translation))
        return {};
    wchar_t key[64];
    swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\FileDescription",
             translations[0].language, translations[0].codePage);
    wchar_t* text = nullptr;
    UINT chars = 0;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &chars) || !text ||
        chars == 0)
        return {};
    return QString::fromWCharArray(text).trimmed();
}

/// Every executable that owns an audio session on an active playback device.
/// Runs on its own thread so that COM is initialised as this code needs,
/// whatever apartment the server's thread is in.
QMap<QString, Entry> enumerateSessions()
{
    QMap<QString, Entry> out;
    std::thread worker([&out] {
        const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDeviceCollection> devices;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           IID_PPV_ARGS(&enumerator))) &&
                SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
                const DWORD self = GetCurrentProcessId();
                UINT count = 0;
                devices->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<IMMDevice> device;
                    ComPtr<IAudioSessionManager2> manager;
                    ComPtr<IAudioSessionEnumerator> sessions;
                    if (FAILED(devices->Item(i, &device)) ||
                        FAILED(
                            device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(manager.GetAddressOf()))) ||
                        FAILED(manager->GetSessionEnumerator(&sessions)))
                        continue;
                    int n = 0;
                    sessions->GetCount(&n);
                    for (int s = 0; s < n; ++s) {
                        ComPtr<IAudioSessionControl> control;
                        ComPtr<IAudioSessionControl2> control2;
                        if (FAILED(sessions->GetSession(s, &control)) ||
                            FAILED(control.As(&control2)))
                            continue;
                        if (control2->IsSystemSoundsSession() == S_OK) continue;
                        AudioSessionState state = AudioSessionStateInactive;
                        control2->GetState(&state);
                        if (state == AudioSessionStateExpired) continue;
                        DWORD pid = 0;
                        if (FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == self)
                            continue;
                        const QString path = processPath(pid);
                        if (path.isEmpty()) continue;
                        const QString exe = QFileInfo(path).fileName().toLower();
                        Entry& e = out[exe];
                        e.exe = exe;
                        if (e.path.isEmpty()) e.path = path;
                        if (state == AudioSessionStateActive) e.playing = true;
                    }
                }
            }
        }
        if (SUCCEEDED(coInit)) CoUninitialize();
    });
    worker.join();
    return out;
}

#endif

} // namespace

bool perAppCaptureSupported()
{
#ifdef Q_OS_WIN
    // RtlGetVersion tells the truth where GetVersionEx lies to an unmanifested
    // process. Process loopback arrived in build 20348.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const bool supported = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn) return false;
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (fn(&info) != 0) return false;
        return info.dwMajorVersion > 10 ||
               (info.dwMajorVersion == 10 && info.dwBuildNumber >= 20348);
    }();
    return supported;
#else
    return false;
#endif
}

QJsonObject list(const QStringList& excluded)
{
    QJsonObject result;
    const bool supported = perAppCaptureSupported();
    result[QStringLiteral("supported")] = supported;
#ifdef Q_OS_WIN
    if (!supported)
        result[QStringLiteral("reason")] =
            QStringLiteral("Per-app audio needs Windows 11 on the streaming PC.");
    QMap<QString, Entry> found = enumerateSessions();
#else
    result[QStringLiteral("reason")] =
        QStringLiteral("Per-app audio is only available on a Windows host.");
    QMap<QString, Entry> found;
#endif

    QJsonArray sources;
    QStringList listed;
    {
        QMutexLocker lock(&g_CacheMutex);
        for (auto it = found.begin(); it != found.end(); ++it) {
            Entry& e = it.value();
            Cached cached = g_Cache.value(e.path);
            if (cached.name.isEmpty()) {
#ifdef Q_OS_WIN
                cached.name = fileDescription(e.path);
#endif
                if (cached.name.isEmpty()) cached.name = nameFromExe(e.exe);
                cached.icon = pngDataUrl(HostAppImage::iconOf(e.path, 48));
                g_Cache.insert(e.path, cached);
            }
            g_ByExe.insert(e.exe, cached);
            QJsonObject o;
            o[QStringLiteral("exe")] = e.exe;
            o[QStringLiteral("name")] = cached.name;
            o[QStringLiteral("icon")] = cached.icon;
            o[QStringLiteral("running")] = true;
            o[QStringLiteral("playing")] = e.playing;
            o[QStringLiteral("excluded")] = excluded.contains(e.exe);
            sources.append(o);
            listed.append(e.exe);
        }
        // Excluded apps that are not playing anything right now still belong
        // on the list: unticking one must always be possible.
        for (const QString& exe : excluded) {
            if (listed.contains(exe)) continue;
            const Cached cached = g_ByExe.value(exe);
            QJsonObject o;
            o[QStringLiteral("exe")] = exe;
            o[QStringLiteral("name")] = cached.name.isEmpty() ? nameFromExe(exe) : cached.name;
            o[QStringLiteral("icon")] = cached.icon;
            o[QStringLiteral("running")] = false;
            o[QStringLiteral("playing")] = false;
            o[QStringLiteral("excluded")] = true;
            sources.append(o);
        }
    }
    result[QStringLiteral("sources")] = sources;
    return result;
}

} // namespace AudioSources
