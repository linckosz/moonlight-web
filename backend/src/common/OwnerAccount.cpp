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

#include "OwnerAccount.h"
#include "Edition.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>
#endif

namespace mw::owneraccount {

#ifdef Q_OS_WIN
namespace {

const wchar_t kValue[] = L"OwnerSid";

HKEY rootOf(const Store& store)
{
    return store.hive == Store::Hive::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

/// A SID read from its text form, to be freed with LocalFree; null when the
/// text is not one.
PSID sidFromText(const QString& text)
{
    if (text.isEmpty()) return nullptr;
    PSID sid = nullptr;
    if (!::ConvertStringSidToSidW(text.toStdWString().c_str(), &sid)) return nullptr;
    return sid;
}

} // namespace
#endif

Store machineStore()
{
    // Where the installer keeps what it knows of this installation: the key
    // its own values live under (installer/moonlightweb.iss, VDisplaySetupKey)
    return {Store::Hive::Machine,
            QStringLiteral("SOFTWARE\\") + mw::edition::productName() + QStringLiteral("\\Setup")};
}

bool record(const Store& store, QString* why)
{
    const auto fail = [why](const QString& reason) {
        if (why) *why = reason;
        return false;
    };
#ifdef Q_OS_WIN
    // Without a key the value would land on the hive's root
    if (store.key.isEmpty()) return fail(QStringLiteral("no key to record the account under"));
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return fail(QStringLiteral("this process's account cannot be read, error %1")
                        .arg(::GetLastError()));
    const QString sid = accountOf(token);
    ::CloseHandle(token);
    if (sid.isEmpty()) return fail(QStringLiteral("this process's account cannot be read"));
    // An installer the system runs (a service updating itself, a deployment
    // tool) is nobody's: what an earlier install recorded stays
    if (isServiceAccount(sid))
        return fail(QStringLiteral("this process runs as the system, which is nobody's account"));

    HKEY key = nullptr;
    LSTATUS status = ::RegCreateKeyExW(rootOf(store), store.key.toStdWString().c_str(), 0, nullptr,
                                       0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS)
        return fail(
            QStringLiteral("the installation's key cannot be opened, error %1").arg(status));
    const std::wstring text = sid.toStdWString();
    status = ::RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                              static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        return fail(QStringLiteral("the account cannot be recorded, error %1").arg(status));
    return true;
#else
    Q_UNUSED(store);
    return fail(QStringLiteral("not on this system"));
#endif
}

void forget(const Store& store)
{
#ifdef Q_OS_WIN
    if (store.key.isEmpty()) return;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(rootOf(store), store.key.toStdWString().c_str(), 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS)
        return;
    ::RegDeleteValueW(key, kValue);
    ::RegCloseKey(key);
#else
    Q_UNUSED(store);
#endif
}

QString recorded(const Store& store)
{
#ifdef Q_OS_WIN
    if (store.key.isEmpty()) return {};
    wchar_t text[256];
    DWORD size = sizeof(text);
    if (::RegGetValueW(rootOf(store), store.key.toStdWString().c_str(), kValue, RRF_RT_REG_SZ,
                       nullptr, text, &size) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(text);
#else
    Q_UNUSED(store);
    return {};
#endif
}

QString accountOf(void* token)
{
#ifdef Q_OS_WIN
    if (!token) return {};
    DWORD size = 0;
    ::GetTokenInformation(static_cast<HANDLE>(token), TokenUser, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<unsigned char> buffer(size);
    if (!::GetTokenInformation(static_cast<HANDLE>(token), TokenUser, buffer.data(), size, &size))
        return {};
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid,
                                  &text))
        return {};
    const QString sid = QString::fromWCharArray(text);
    ::LocalFree(text);
    return sid;
#else
    Q_UNUSED(token);
    return {};
#endif
}

bool same(const QString& recordedSid, const QString& accountSid)
{
#ifdef Q_OS_WIN
    const PSID a = sidFromText(recordedSid);
    const PSID b = sidFromText(accountSid);
    const bool equal = a && b && ::EqualSid(a, b) != 0;
    if (a) ::LocalFree(a);
    if (b) ::LocalFree(b);
    return equal;
#else
    Q_UNUSED(recordedSid);
    Q_UNUSED(accountSid);
    return false;
#endif
}

bool isServiceAccount(const QString& sid)
{
#ifdef Q_OS_WIN
    const PSID parsed = sidFromText(sid);
    if (!parsed) return false;
    const bool service = ::IsWellKnownSid(parsed, WinLocalSystemSid) ||
                         ::IsWellKnownSid(parsed, WinLocalServiceSid) ||
                         ::IsWellKnownSid(parsed, WinNetworkServiceSid);
    ::LocalFree(parsed);
    return service;
#else
    Q_UNUSED(sid);
    return false;
#endif
}

bool isOwner(const Store& store, void* token)
{
    const QString account = accountOf(token);
    return !isServiceAccount(account) && same(recorded(store), account);
}

} // namespace mw::owneraccount
