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

#include "server/QuitServer.h"

#include "common/Edition.h"
#include "network/RouterHoles.h"
#include "server/LocalChannel.h"

#include <QByteArray>
#include <QDir>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>

#include <windows.h>
#include <shlobj.h>

namespace mw::quitserver {

namespace {

constexpr char kVerb[] = "--quit-server";
constexpr wchar_t kRecordName[] = L"router_holes.json";

Count refused(const QString& why)
{
    Count c;
    c.why = why;
    return c;
}

/// A folder on the way to the record, as it is now: there and a real folder
/// (Real), or not there at all (Absent), or something else (Refused).
enum class Folder
{
    Real,
    Absent,
    Refused,
};

/// Looked at without following it: a reparse point (a junction, a symbolic
/// link, a mount point) is refused, whatever it leads to. Attributes only.
Folder lookAt(const std::wstring& path, QString* why)
{
    HANDLE h = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return Folder::Absent;
        *why = QStringLiteral("a folder on the way cannot be looked at (%1)").arg(code);
        return Folder::Refused;
    }
    BY_HANDLE_FILE_INFORMATION info = {};
    const bool read = ::GetFileInformationByHandle(h, &info) != FALSE;
    ::CloseHandle(h);
    if (!read) {
        *why = QStringLiteral("a folder on the way cannot be read");
        return Folder::Refused;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *why = QStringLiteral("a folder on the way is a link");
        return Folder::Refused;
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *why = QStringLiteral("a folder on the way is not a folder");
        return Folder::Refused;
    }
    return Folder::Real;
}

/// The path as the system writes it back: long names, no "\\?\" in front.
QString finalPathOf(HANDLE h)
{
    wchar_t buffer[MAX_PATH * 4];
    const DWORD size = ::GetFinalPathNameByHandleW(h, buffer, static_cast<DWORD>(std::size(buffer)),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (size == 0 || size >= std::size(buffer)) return {};
    QString path = QString::fromWCharArray(buffer, static_cast<int>(size));
    if (path.startsWith(QLatin1String("\\\\?\\UNC\\")))
        path = QStringLiteral("\\\\") + path.mid(8);
    else if (path.startsWith(QLatin1String("\\\\?\\")))
        path = path.mid(4);
    return path;
}

/// The long form of a path that exists (no 8.3 names), as the final path of
/// a handle comes back in. Empty when it cannot be had.
QString longPathOf(const std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 4];
    const DWORD size =
        ::GetLongPathNameW(path.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0 || size >= std::size(buffer)) return {};
    return QString::fromWCharArray(buffer, static_cast<int>(size));
}

QString nameOf(Code code)
{
    switch (code) {
    case Code::Gone: return QStringLiteral("the server quit; nothing is left on the router");
    case Code::LinesLeft:
        return QStringLiteral("the server quit; some of its router entries are left");
    case Code::NobodyToAsk: return QStringLiteral("no server of this install answered");
    case Code::StillRunning:
        return QStringLiteral("the server was asked to quit and is still running");
    case Code::Refused: return QStringLiteral("the server refused to quit");
    case Code::RecordUnreadable:
        return QStringLiteral("the record of router entries is unreadable");
    case Code::NobodyLinesLeft:
        return QStringLiteral("no server of this install answered; some router entries are left");
    case Code::GoneOtherAccount:
        return QStringLiteral("the server quit; it ran under another account, whose record is not "
                              "read");
    }
    return {};
}

} // namespace

Count countRecordLines(const QString& roaming, const QString& dataName,
                       const std::function<void()>& betweenChecks)
{
    if (roaming.isEmpty() || dataName.isEmpty())
        return refused(QStringLiteral("no folder to read"));
    // Every folder from the profile's AppData down: the roaming folder's
    // parent, the roaming folder, MoonlightWeb, this install's.
    const QString root = QDir::toNativeSeparators(roaming);
    const QString parent = root.section(QLatin1Char('\\'), 0, -2);
    const QString org = root + QStringLiteral("\\MoonlightWeb");
    const QString mine = org + QLatin1Char('\\') + dataName;
    QString why;
    for (const QString& folder : {parent, root}) {
        if (folder.isEmpty()) continue;
        if (lookAt(folder.toStdWString(), &why) != Folder::Real) {
            if (why.isEmpty()) why = QStringLiteral("the application data folder is not there");
            return refused(why);
        }
    }
    for (const QString& folder : {org, mine}) {
        switch (lookAt(folder.toStdWString(), &why)) {
        case Folder::Real: break;
        case Folder::Absent: {
            Count none;
            none.read = true;
            return none;
        }
        case Folder::Refused: return refused(why);
        }
    }
    // Where the file must turn out to be, in the form the system gives back.
    const QString expected = longPathOf(mine.toStdWString());
    if (expected.isEmpty()) return refused(QStringLiteral("the record's folder cannot be named"));

    if (betweenChecks) betweenChecks();

    const std::wstring file = (mine + QLatin1Char('\\')).toStdWString() + kRecordName;
    HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            Count none;
            none.read = true;
            return none;
        }
        return refused(QStringLiteral("the record cannot be opened (%1)").arg(code));
    }
    const auto close = [h]() { ::CloseHandle(h); };

    BY_HANDLE_FILE_INFORMATION info = {};
    if (!::GetFileInformationByHandle(h, &info) || ::GetFileType(h) != FILE_TYPE_DISK) {
        close();
        return refused(QStringLiteral("the record cannot be read"));
    }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) {
        close();
        return refused(QStringLiteral("the record is not a plain file"));
    }
    // A folder swapped for a link once it was checked: the file opened is
    // not the one asked for.
    const QString got = finalPathOf(h);
    const QString wanted = expected + QLatin1Char('\\') + QString::fromWCharArray(kRecordName);
    if (got.isEmpty() || got.compare(wanted, Qt::CaseInsensitive) != 0) {
        close();
        return refused(QStringLiteral("the record is not where it was asked for"));
    }
    const quint64 size = (static_cast<quint64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > static_cast<quint64>(mw::routerholes::kFileMax)) {
        close();
        return refused(QStringLiteral("the record is larger than a record may be"));
    }
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    DWORD readBytes = 0;
    const bool ok =
        size == 0 || (::ReadFile(h, bytes.data(), static_cast<DWORD>(size), &readBytes, nullptr) &&
                      readBytes == size);
    close();
    if (!ok) return refused(QStringLiteral("the record cannot be read"));

    // In memory, by the parser the server reads it with: its lines, this kind
    // of instance's and the other's.
    mw::routerholes::Content content;
    if (!mw::routerholes::parse(bytes, mw::edition::isDev(), content))
        return refused(QStringLiteral("the record does not read as one"));
    Count count;
    count.read = true;
    count.lines = static_cast<int>(content.holes.size() + content.others.size());
    return count;
}

QString roamingAppData()
{
    PWSTR path = nullptr;
    QString out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path)))
        out = QString::fromWCharArray(path);
    if (path) ::CoTaskMemFree(path);
    return out;
}

bool sameAccount(void* process)
{
    const auto userOf = [](HANDLE token, std::vector<unsigned char>& buffer) -> PSID {
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size == 0) return nullptr;
        buffer.resize(size);
        if (!::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return nullptr;
        return reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    };
    HANDLE theirs = nullptr;
    HANDLE mine = nullptr;
    if (!process || !::OpenProcessToken(static_cast<HANDLE>(process), TOKEN_QUERY, &theirs))
        return false;
    bool same = false;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &mine)) {
        std::vector<unsigned char> a;
        std::vector<unsigned char> b;
        const PSID them = userOf(theirs, a);
        const PSID me = userOf(mine, b);
        same = them && me && ::EqualSid(them, me);
        ::CloseHandle(mine);
    }
    ::CloseHandle(theirs);
    return same;
}

Code run(const QString& endpoint, const QString& roaming, const QString& dataName,
         const Limits& limits, QString* said, const std::function<bool(void*)>& sameAs)
{
    const auto say = [said](const QString& line) {
        if (said) *said = line;
    };
    // What the record says, once nothing can be writing it any more.
    const auto afterwards = [&](Code none, Code some) {
        const Count count = countRecordLines(roaming, dataName);
        if (!count.read) {
            say(count.why);
            return Code::RecordUnreadable;
        }
        return count.lines > 0 ? some : none;
    };

    mw::localchannel::Request request;
    request.method = QStringLiteral("POST");
    request.path = QStringLiteral("/api/system/quit");
    request.body = QByteArrayLiteral("{}");
    void* process = nullptr;
    QString why;
    const mw::localchannel::Reply reply =
        mw::localchannel::requestHoldingServer(endpoint, request, limits.channelMs, &process, &why);
    if (reply.status == 0) {
        say(why);
        return afterwards(Code::NobodyToAsk, Code::NobodyLinesLeft);
    }
    if (reply.status < 200 || reply.status >= 300) {
        if (process) ::CloseHandle(static_cast<HANDLE>(process));
        say(QStringLiteral("HTTP %1").arg(reply.status));
        return Code::Refused;
    }
    // Asked while it still runs: a token is not read off a process that is
    // gone. Another account's server (a service, reached by the people signed
    // in) keeps its record in that account's profile, not in this one.
    const bool ours = sameAs ? sameAs(process) : sameAccount(process);
    const DWORD waited =
        ::WaitForSingleObject(static_cast<HANDLE>(process), static_cast<DWORD>(limits.exitMs));
    ::CloseHandle(static_cast<HANDLE>(process));
    if (waited != WAIT_OBJECT_0) return Code::StillRunning;
    if (!ours) return Code::GoneOtherAccount;
    return afterwards(Code::Gone, Code::LinesLeft);
}

bool isTheVerb(int argc, char** argv)
{
    return argc == 2 && argv && argv[1] && std::strcmp(argv[1], kVerb) == 0;
}

int runVerb()
{
    const Limits limits;
    // Whatever happens below, this process ends in time: the installer
    // waits for it, and must not be held.
    std::thread([ms = limits.watchdogMs]() {
        ::Sleep(static_cast<DWORD>(ms));
        std::fputs("quit-server: gave up at the limit\n", stderr);
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(Code::StillRunning));
    }).detach();

    // The channel of this install: its name comes from the edition the
    // program was built as, and reads no setting.
    const QString endpoint = mw::localchannel::endpointFor(
        mw::localchannel::channelName(mw::edition::productName(), false), QString());
    QString said;
    const Code code = run(endpoint, roamingAppData(), mw::edition::dataName(), limits, &said);
    QString line = QStringLiteral("quit-server: ") + nameOf(code);
    if (!said.isEmpty()) line += QStringLiteral(" (") + said + QLatin1Char(')');
    std::fputs(line.toLocal8Bit().constData(), stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
    return static_cast<int>(code);
}

} // namespace mw::quitserver
