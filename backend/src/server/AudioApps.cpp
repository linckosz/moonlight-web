/*
 * MoonlightWeb
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

#include "AudioApps.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QMap>
#include <QString>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "mw/native/AudioSource.h"

#include <iterator>
#include <thread>
#include <vector>
#endif

namespace AudioApps {

#ifdef Q_OS_WIN

namespace {

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

/// "chrome.exe" → "Chrome": what to call an app whose file says nothing better.
QString nameFromExe(const QString& exe)
{
    QString base = exe;
    if (base.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) base.chop(4);
    if (!base.isEmpty()) base[0] = base[0].toUpper();
    return base;
}

/// Lower-case executable → full path, for each app with an audio session on an
/// active playback device. Runs on its own thread so that COM is initialised as
/// this code needs, whatever apartment the server's thread is in.
QMap<QString, QString> enumerateSessions()
{
    QMap<QString, QString> out;
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
                        out.insert(QFileInfo(path).fileName().toLower(), path);
                    }
                }
            }
        }
        if (SUCCEEDED(coInit)) CoUninitialize();
    });
    worker.join();
    return out;
}

} // namespace

QJsonArray list()
{
    const QMap<QString, QString> sessions = enumerateSessions();
    const QString self = QFileInfo(processPath(GetCurrentProcessId())).fileName().toLower();
    // Sorted by the name shown; the executable breaks a tie and keeps two apps
    // of the same name apart.
    QMap<QString, QJsonObject> sorted;
    for (auto it = sessions.begin(); it != sessions.end(); ++it) {
        const QString& exe = it.key();
        if (exe == self || mw::native::isWindowsShellExe(exe.toStdString())) continue;
        QString name = fileDescription(it.value());
        if (name.isEmpty()) name = nameFromExe(exe);
        QJsonObject app;
        app[QStringLiteral("exe")] = exe;
        app[QStringLiteral("name")] = name;
        sorted.insert(name.toLower() + QChar(0) + exe, app);
    }
    QJsonArray out;
    for (const QJsonObject& app : sorted)
        out.append(app);
    return out;
}

#else

QJsonArray list()
{
    return {};
}

#endif

} // namespace AudioApps
