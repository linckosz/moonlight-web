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

#include "PrivilegedPlace.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>
// clang-format on
#include <fcntl.h>
#include <io.h>

#include <string>
#include <vector>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mw::place {

namespace {

const char* const kTag = "[PrivilegedPlace]";

/// The TNR's stand-in for the process's own token (setForTests()).
struct TestState
{
    bool active = false;
    bool privileged = true;
    QString installDir;
    QStringList trustedSids;
};

QMutex g_Mutex;
TestState g_Test;

TestState testState()
{
    QMutexLocker lock(&g_Mutex);
    return g_Test;
}

/// One line on stderr: the only place left when there is no log to write to.
void say(const QString& line)
{
    std::fprintf(stderr, "%s %s\n", kTag, qPrintable(line));
    std::fflush(stderr);
}

#ifdef Q_OS_WIN

std::wstring native(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

QString lastError(DWORD err = GetLastError())
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    const QString text = buf ? QString::fromWCharArray(buf).trimmed() : QString();
    LocalFree(buf);
    return QStringLiteral("%1 (%2)").arg(text).arg(err);
}

/// A handle closed when it goes out of scope.
struct Handle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle)
        : h(handle)
    {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle()
    {
        if (valid()) CloseHandle(h);
    }
    bool valid() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
    HANDLE release()
    {
        HANDLE out = h;
        h = INVALID_HANDLE_VALUE;
        return out;
    }
};

/// A SID string as the system writes it back, empty when @p text is not one.
QString canonicalSid(const QString& text)
{
    if (text.isEmpty()) return {};
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(text.toStdWString().c_str(), &sid)) return {};
    wchar_t* str = nullptr;
    QString out;
    if (ConvertSidToStringSidW(sid, &str)) {
        out = QString::fromWCharArray(str);
        LocalFree(str);
    }
    LocalFree(sid);
    return out;
}

/// The token's user SID and its elevation, read once.
struct TokenFacts
{
    bool read = false;
    bool serviceAccount = false;
    bool elevated = false;
    QString userSid;
};

TokenFacts readToken()
{
    TokenFacts facts;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return facts;
    Handle token(raw);

    DWORD len = 0;
    GetTokenInformation(token.h, TokenUser, nullptr, 0, &len);
    if (len == 0) return facts;
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(token.h, TokenUser, buf.data(), len, &len)) return facts;
    PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    facts.serviceAccount = IsWellKnownSid(user, WinLocalSystemSid) ||
                           IsWellKnownSid(user, WinLocalServiceSid) ||
                           IsWellKnownSid(user, WinNetworkServiceSid);
    wchar_t* str = nullptr;
    if (ConvertSidToStringSidW(user, &str)) {
        facts.userSid = QString::fromWCharArray(str);
        LocalFree(str);
    }

    TOKEN_ELEVATION elevation{};
    DWORD got = 0;
    if (!GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &got))
        return facts;
    facts.elevated = elevation.TokenIsElevated != 0;
    facts.read = !facts.userSid.isEmpty();
    return facts;
}

const TokenFacts& tokenFacts()
{
    static const TokenFacts facts = readToken();
    return facts;
}

#endif // Q_OS_WIN

bool realPrivileged()
{
#ifdef Q_OS_WIN
    const TokenFacts& facts = tokenFacts();
    // A token that cannot be read is taken as privileged: the process then
    // writes only where a privileged one may.
    if (!facts.read) return true;
    return facts.serviceAccount || facts.elevated;
#else
    return geteuid() == 0 || geteuid() != getuid();
#endif
}

} // namespace

bool isPrivileged()
{
    const TestState test = testState();
    if (test.active) return test.privileged;
    static const bool privileged = realPrivileged();
    return privileged;
}

QString currentUserSid()
{
#ifdef Q_OS_WIN
    return tokenFacts().userSid;
#else
    return {};
#endif
}

QString userLocalDataDir()
{
#ifdef Q_OS_WIN
    PWSTR path = nullptr;
    QString dir;
    if (SUCCEEDED(
            SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &path)) &&
        path)
        dir = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    CoTaskMemFree(path);
    return dir;
#else
    return {};
#endif
}

void setForTests(const QString& installDir, const QStringList& trustedSids, bool privileged)
{
    QMutexLocker lock(&g_Mutex);
    g_Test.active = true;
    g_Test.privileged = privileged;
    g_Test.installDir = installDir;
    g_Test.trustedSids = trustedSids;
}

void resetForTests()
{
    QMutexLocker lock(&g_Mutex);
    g_Test = TestState();
}

#ifdef Q_OS_WIN
namespace win {

QString describe(Check check)
{
    switch (check) {
    case Check::Ok: return QStringLiteral("ok");
    case Check::Missing: return QStringLiteral("missing");
    case Check::ReparsePoint:
        return QStringLiteral("a reparse point or a path that resolves elsewhere");
    case Check::NotDirectory: return QStringLiteral("not a directory");
    case Check::Owner: return QStringLiteral("owned by another account");
    case Check::Acl: return QStringLiteral("its DACL lets another account write");
    case Check::Error: return QStringLiteral("its security could not be read");
    }
    return QStringLiteral("unknown");
}

QStringList defaultTrustedSids()
{
    return {QStringLiteral("S-1-5-32-544"), QStringLiteral("S-1-5-18")};
}

QString lockedSddl(Dir dir, const QStringList& trustedSids, const QString& readerSid)
{
    if (trustedSids.isEmpty()) return {};
    // Owner: the first trusted account. Protected DACL: nothing inherited from
    // the directory above. Full control, inherited by what is made inside, for
    // each trusted account.
    QString sddl = QStringLiteral("O:%1D:P").arg(trustedSids.first());
    for (const QString& sid : trustedSids)
        sddl += QStringLiteral("(A;OICI;FA;;;%1)").arg(sid);
    switch (dir) {
    case Dir::HostData:
        // Listed and traversed by any signed-in account, on this directory
        // only, so a user's own logs can be reached below it.
        sddl += QStringLiteral("(A;;FRFX;;;AU)");
        break;
    case Dir::UserLogs:
        // Read by that account, on the directory and the files made in it.
        if (!readerSid.isEmpty()) sddl += QStringLiteral("(A;OICI;FRFX;;;%1)").arg(readerSid);
        break;
    case Dir::Crashes: break;
    }
    return sddl;
}

namespace {

/// SIDs parsed once for comparing against an ACL.
class SidSet
{
public:
    explicit SidSet(const QStringList& sids)
    {
        for (const QString& text : sids) {
            PSID sid = nullptr;
            if (!ConvertStringSidToSidW(text.toStdWString().c_str(), &sid)) continue;
            const BYTE* bytes = static_cast<const BYTE*>(sid);
            m_Sids.emplace_back(bytes, bytes + GetLengthSid(sid));
            LocalFree(sid);
        }
    }
    bool contains(PSID sid) const
    {
        for (const auto& s : m_Sids)
            if (EqualSid(const_cast<BYTE*>(s.data()), sid)) return true;
        return false;
    }

private:
    std::vector<std::vector<BYTE>> m_Sids;
};

/// The path the file system resolves @p h to, without the \\?\ prefix.
QString finalPath(HANDLE h)
{
    DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    n = GetFinalPathNameByHandleW(h, buf.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    QString path = QString::fromStdWString(buf);
    if (path.startsWith(QLatin1String("\\\\?\\UNC\\")))
        path = QStringLiteral("\\\\") + path.mid(8);
    else if (path.startsWith(QLatin1String("\\\\?\\")))
        path = path.mid(4);
    return path;
}

QString comparable(const QString& path)
{
    QString p = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    while (p.endsWith(QLatin1Char('\\')) && p.size() > 3)
        p.chop(1);
    return p.toLower();
}

bool samePath(const QString& a, const QString& b)
{
    return !a.isEmpty() && comparable(a) == comparable(b);
}

/// Rights that write, add, delete or re-ACL, refused to anyone untrusted on a
/// directory made here.
constexpr ACCESS_MASK kLockedWrite =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES |
    DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL | GENERIC_WRITE | MAXIMUM_ALLOWED;
/// On the install directory itself: renaming, deleting, re-ACLing it, or adding
/// to it.
constexpr ACCESS_MASK kInstallWrite =
    DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
    GENERIC_ALL | GENERIC_WRITE | GENERIC_READ | GENERIC_EXECUTE | MAXIMUM_ALLOWED;

const wchar_t* const kTrustedInstaller =
    L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

/// The owner and DACL of the object @p h is open on, judged by @p locked's
/// rules (a directory made here) or the install directory's.
Check checkSecurity(HANDLE h, const SidSet& trusted, bool locked)
{
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS)
        return Check::Error;
    struct Free
    {
        PSECURITY_DESCRIPTOR sd;
        ~Free() { LocalFree(sd); }
    } freeSd{sd};

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(sd, &control, &revision)) return Check::Error;
    if (locked && !(control & SE_DACL_PROTECTED)) return Check::Acl;
    if (!dacl) return Check::Acl; // no DACL: everyone has every right
    if (!owner || !trusted.contains(owner)) return Check::Owner;

    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER* ace = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void**>(&ace))) return Check::Error;
        if (!locked) {
            // The install directory: only what applies to the directory
            // itself, and a deny only takes rights away.
            if (ace->AceFlags & INHERIT_ONLY_ACE) continue;
            if (ace->AceType == ACCESS_DENIED_ACE_TYPE) continue;
            if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE &&
                ace->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
                return Check::Acl;
        } else if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            // A directory made here holds plain allow ACEs and nothing else.
            return Check::Acl;
        }
        // ACCESS_ALLOWED_ACE and ACCESS_ALLOWED_CALLBACK_ACE share this layout.
        auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace);
        PSID sid = reinterpret_cast<PSID>(&allowed->SidStart);
        const ACCESS_MASK refused = locked ? kLockedWrite : kInstallWrite;
        if ((allowed->Mask & refused) && !trusted.contains(sid)) return Check::Acl;
    }
    return Check::Ok;
}

/// Whether the object @p h is open on belongs to SYSTEM, Administrators,
/// TrustedInstaller or one of @p trustedSids.
bool ownedByTrusted(HANDLE h, const QStringList& trustedSids)
{
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                        nullptr, &sd) != ERROR_SUCCESS)
        return false;
    QStringList owners = trustedSids;
    owners << QStringLiteral("S-1-5-18") << QStringLiteral("S-1-5-32-544")
           << QString::fromWCharArray(kTrustedInstaller);
    const bool trusted = owner && SidSet(owners).contains(owner);
    LocalFree(sd);
    return trusted;
}

Check openError(DWORD err)
{
    return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? Check::Missing
                                                                        : Check::Error;
}

} // namespace

Check checkLockedDir(const QString& path, const QStringList& trustedSids)
{
    // Opened as itself: a reparse point is seen, never followed.
    Handle h(CreateFileW(native(path).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr));
    if (!h.valid()) return openError(GetLastError());

    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag)))
        return Check::Error;
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Check::ReparsePoint;
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return Check::NotDirectory;
    if (!samePath(finalPath(h.h), path)) return Check::ReparsePoint;

    return checkSecurity(h.h, SidSet(trustedSids), /*locked=*/true);
}

Check checkInstallDir(const QString& path, const QStringList& trustedSids, QString* resolved)
{
    Handle h(CreateFileW(native(path).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid()) return openError(GetLastError());

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.h, &info)) return Check::Error;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return Check::NotDirectory;
    const QString final = finalPath(h.h);
    if (final.isEmpty()) return Check::Error;

    QStringList owners = trustedSids;
    owners << QStringLiteral("S-1-5-18") << QStringLiteral("S-1-5-32-544")
           << QString::fromWCharArray(kTrustedInstaller);
    const Check check = checkSecurity(h.h, SidSet(owners), /*locked=*/false);
    if (check == Check::Ok && resolved) *resolved = final;
    return check;
}

} // namespace win

namespace {

bool clearReadOnly(HANDLE h)
{
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic))) return false;
    if (!(basic.FileAttributes & FILE_ATTRIBUTE_READONLY)) return true;
    basic.FileAttributes &= ~DWORD(FILE_ATTRIBUTE_READONLY);
    if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    return SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof(basic)) != 0;
}

/// Mark the object @p h is open on for deletion. Its name goes at once where
/// the file system allows it, so its directory can be deleted right after.
bool deleteByHandle(HANDLE h)
{
#ifdef FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
    FILE_DISPOSITION_INFO_EX ex{};
    ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof(ex))) return true;
#endif
    FILE_DISPOSITION_INFO info{};
    info.DeleteFile = TRUE;
    return SetFileInformationByHandle(h, FileDispositionInfo, &info, sizeof(info)) != 0;
}

/// The names in the directory @p dir is open on, read through that handle.
bool listNames(HANDLE dir, std::vector<std::wstring>* names)
{
    std::vector<unsigned long long> buf(64 * 1024 / sizeof(unsigned long long));
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(dir, cls, buf.data(),
                                          DWORD(buf.size() * sizeof(buf[0])))) {
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
        cls = FileIdBothDirectoryInfo;
        auto* entry = reinterpret_cast<const BYTE*>(buf.data());
        for (;;) {
            auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(entry);
            std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") names->push_back(std::move(name));
            if (info->NextEntryOffset == 0) break;
            entry += info->NextEntryOffset;
        }
    }
}

/// Whether the two handles are open on the same file or directory.
bool sameObject(HANDLE a, HANDLE b)
{
    BY_HANDLE_FILE_INFORMATION ia{};
    BY_HANDLE_FILE_INFORMATION ib{};
    if (!GetFileInformationByHandle(a, &ia) || !GetFileInformationByHandle(b, &ib)) return false;
    return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
           ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
}

bool removeEntry(const std::wstring& path)
{
    // Opened as itself, never followed: a junction or a link is deleted as
    // the link. No FILE_SHARE_DELETE, so the directory stays where it is while
    // what is inside it is deleted.
    auto open = [&path](DWORD access) {
        return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    };
    // With the right to clear a read-only attribute when it is granted.
    bool attributes = true;
    HANDLE raw = open(DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE);
    if (raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        attributes = false;
        raw = open(DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    }
    Handle h(raw);
    if (!h.valid()) {
        const DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }

    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag))) return false;
    const bool reparse = tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !reparse) {
        // The listing comes from this very directory: a second handle, opened
        // as itself, that must be on the same object as the first.
        Handle list(CreateFileW(
            path.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!list.valid() || !sameObject(h.h, list.h)) return false;
        std::vector<std::wstring> names;
        if (!listNames(list.h, &names)) return false;
        for (const std::wstring& name : names)
            if (!removeEntry(path + L"\\" + name)) return false;
    }
    if (attributes && !clearReadOnly(h.h)) return false;
    return deleteByHandle(h.h);
}

} // namespace

bool removeNoFollow(const QString& path)
{
    return removeEntry(native(QDir::cleanPath(QFileInfo(path).absoluteFilePath())));
}

namespace {

QStringList trustedSids()
{
    const TestState test = testState();
    return test.active ? test.trustedSids : win::defaultTrustedSids();
}

QString installRequested()
{
    const TestState test = testState();
    return test.active ? test.installDir : QCoreApplication::applicationDirPath();
}

/// The install directory as the file system resolves it, empty when it fails
/// its check (said on stderr, unless @p quiet).
QString installBase(bool quiet = false)
{
    const QString requested = installRequested();
    QString resolved;
    const win::Check check = win::checkInstallDir(requested, trustedSids(), &resolved);
    if (check != win::Check::Ok) {
        if (!quiet)
            say(QStringLiteral("%1 is %2: no file is written under it")
                    .arg(QDir::toNativeSeparators(requested), win::describe(check)));
        return {};
    }
    return resolved;
}

QString hostDataUnder(const QString& base)
{
    return base + QStringLiteral("\\host-data");
}

/// @p path checked, or made with its DACL when missing. An entry that does not
/// check out is deleted (never followed) and made once more; a second failure
/// is a refusal.
bool ensureLocked(const QString& path, win::Dir kind, const QString& readerSid,
                  const QStringList& trusted)
{
    const QString shown = QDir::toNativeSeparators(path);
    win::Check check = win::checkLockedDir(path, trusted);
    if (check == win::Check::Ok) return true;
    if (check == win::Check::Error) {
        say(QStringLiteral("%1: %2").arg(shown, win::describe(check)));
        return false;
    }
    if (check != win::Check::Missing) {
        say(QStringLiteral("%1 is %2: it is made again").arg(shown, win::describe(check)));
        if (!removeNoFollow(path)) {
            say(QStringLiteral("cannot delete %1: %2").arg(shown, lastError()));
            return false;
        }
    }

    PSECURITY_DESCRIPTOR sd = nullptr;
    const std::wstring sddl = win::lockedSddl(kind, trusted, readerSid).toStdWString();
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                              nullptr)) {
        say(QStringLiteral("cannot build the DACL of %1: %2").arg(shown, lastError()));
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const BOOL created = CreateDirectoryW(native(path).c_str(), &sa);
    const DWORD err = GetLastError();
    LocalFree(sd);
    if (!created && err != ERROR_ALREADY_EXISTS) {
        say(QStringLiteral("cannot create %1: %2").arg(shown, lastError(err)));
        return false;
    }

    check = win::checkLockedDir(path, trusted);
    if (check != win::Check::Ok) {
        say(QStringLiteral("%1 is %2 once made: no file is written in it")
                .arg(shown, win::describe(check)));
        return false;
    }
    return true;
}

/// host-data\logs\<sid>, checked or made with each directory above it.
QString privilegedLogDir(const QString& readerSid)
{
    QString sid = currentUserSid();
    if (!readerSid.isEmpty()) {
        // An account's SID, as the system writes it: a local or domain
        // account (S-1-5-21-…) or an Azure AD one (S-1-12-1-…). No group and
        // no well-known SID.
        sid = canonicalSid(readerSid);
        if (sid != readerSid || !(sid.startsWith(QLatin1String("S-1-5-21-")) ||
                                  sid.startsWith(QLatin1String("S-1-12-1-")))) {
            say(QStringLiteral("\"%1\" is not an account's SID: no log file").arg(readerSid));
            return {};
        }
    }
    if (sid.isEmpty()) {
        say(QStringLiteral("this process's account cannot be read: no log file"));
        return {};
    }
    const QString base = installBase();
    if (base.isEmpty()) return {};
    const QStringList trusted = trustedSids();
    const QString hostData = hostDataUnder(base);
    const QString logs = hostData + QStringLiteral("\\logs");
    const QString mine = logs + QLatin1Char('\\') + sid;
    if (!ensureLocked(hostData, win::Dir::HostData, {}, trusted) ||
        !ensureLocked(logs, win::Dir::HostData, {}, trusted) ||
        !ensureLocked(mine, win::Dir::UserLogs, sid, trusted))
        return {};
    return QDir::fromNativeSeparators(mine);
}

/// host-data\<leaf>, for SYSTEM and Administrators only, checked or made.
QString privilegedPrivateDir(const QString& leaf)
{
    const QString base = installBase();
    if (base.isEmpty()) return {};
    const QStringList trusted = trustedSids();
    const QString hostData = hostDataUnder(base);
    const QString dir = hostData + QLatin1Char('\\') + leaf;
    if (!ensureLocked(hostData, win::Dir::HostData, {}, trusted) ||
        !ensureLocked(dir, win::Dir::Crashes, {}, trusted))
        return {};
    return QDir::fromNativeSeparators(dir);
}

QString privilegedCrashDir()
{
    return privilegedPrivateDir(QStringLiteral("crashes"));
}

/// @p dir and each directory above it, up to the install directory, are
/// directories made here; the install directory passes its own check.
bool privilegedWritable(const QString& dir)
{
    // Asked at every open of a log: the caller says what it refuses.
    const QString base = installBase(/*quiet=*/true);
    if (base.isEmpty()) return false;
    const QStringList trusted = trustedSids();
    QString current = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    // The install directory itself is not one of them.
    if (win::samePath(current, base)) return false;
    while (!win::samePath(current, base)) {
        if (win::checkLockedDir(current, trusted) != win::Check::Ok) return false;
        const QString parent = QFileInfo(current).absolutePath();
        if (win::samePath(parent, current))
            return false; // the root: not under the install directory
        current = parent;
    }
    return true;
}

/// NtCreateFile, looked up rather than linked: the one call that opens a name
/// relative to a directory handle.
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

NtCreateFileFn ntCreateFile()
{
    static const NtCreateFileFn fn = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtCreateFileFn>(
                           reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")))
                     : nullptr;
    }();
    return fn;
}

/// One name, with nothing that walks to another directory or names a stream.
bool plainName(const QString& name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) return false;
    for (const QChar c : name)
        if (c == QLatin1Char('\\') || c == QLatin1Char('/') || c == QLatin1Char(':')) return false;
    return true;
}

/// @p dir opened as itself, with no FILE_SHARE_DELETE: nothing renames or
/// replaces it while the handle is held. Invalid, and @p why set, when it is
/// a reparse point, not a directory, a path that resolves elsewhere, or (with
/// @p systemOwned) owned by another account than SYSTEM, Administrators,
/// TrustedInstaller or the TNR's stand-ins.
HANDLE openDirItself(const QString& dir, bool systemOwned, QString* why, bool* missing)
{
    *missing = false;
    Handle h(CreateFileW(native(dir).c_str(),
                         FILE_READ_ATTRIBUTES | FILE_TRAVERSE | READ_CONTROL | SYNCHRONIZE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) {
        const DWORD err = GetLastError();
        *missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        *why = QStringLiteral("the directory cannot be opened: %1").arg(lastError(err));
        return INVALID_HANDLE_VALUE;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag))) {
        *why = QStringLiteral("the directory cannot be read: %1").arg(lastError());
        return INVALID_HANDLE_VALUE;
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !win::samePath(win::finalPath(h.h), dir)) {
        *why = QStringLiteral("the directory is a reparse point, or resolves elsewhere");
        return INVALID_HANDLE_VALUE;
    }
    if (systemOwned && !win::ownedByTrusted(h.h, trustedSids())) {
        *why = QStringLiteral("the directory does not belong to SYSTEM or to Administrators");
        return INVALID_HANDLE_VALUE;
    }
    return h.release();
}

enum class Shared
{
    Read,
    Write,
    Delete
};

constexpr NTSTATUS kStatusNameNotFound = NTSTATUS(0xC0000034L);

/// The file @p name of the directory @p dir is open on, opened as itself for
/// @p how. A regular file with a single link (to delete one: any file, or a
/// link as itself). @p missing: there is no such file.
HANDLE openIn(HANDLE dir, const QString& name, Shared how, QString* why, bool* missing)
{
    *missing = false;
    const NtCreateFileFn create = ntCreateFile();
    if (!create || !plainName(name)) {
        *why = QStringLiteral("it cannot be opened by its name in its directory");
        return INVALID_HANDLE_VALUE;
    }
    std::wstring wide = name.toStdWString();
    UNICODE_STRING uname{};
    uname.Buffer = wide.data();
    uname.Length = USHORT(wide.size() * sizeof(wchar_t));
    uname.MaximumLength = uname.Length;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &uname, OBJ_CASE_INSENSITIVE, dir, nullptr);

    ACCESS_MASK access = FILE_GENERIC_READ;
    ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    ULONG disposition = FILE_OPEN;
    if (how == Shared::Write) {
        access = FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES;
        share = FILE_SHARE_READ;
        disposition = FILE_OPEN_IF;
    } else if (how == Shared::Delete) {
        access = DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    }
    HANDLE raw = nullptr;
    IO_STATUS_BLOCK io{};
    const NTSTATUS status =
        create(&raw, access, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
               FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
               nullptr, 0);
    if (status < 0) {
        *missing = status == kStatusNameNotFound;
        *why = QStringLiteral("it cannot be opened (status 0x%1)")
                   .arg(quint32(status), 8, 16, QLatin1Char('0'));
        return INVALID_HANDLE_VALUE;
    }
    Handle h(raw);
    if (how == Shared::Delete) return h.release();

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.h, &info)) {
        *why = lastError();
        return INVALID_HANDLE_VALUE;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *why = QStringLiteral("it is a reparse point");
        return INVALID_HANDLE_VALUE;
    }
    if (info.nNumberOfLinks != 1) {
        *why = QStringLiteral("it has more than one link");
        return INVALID_HANDLE_VALUE;
    }
    return h.release();
}

/// This account's host-data\logs\<SID>, when it is there and checks out.
QString ownHostDataLogs()
{
    const QString sid = currentUserSid();
    const QString base = installBase(/*quiet=*/true);
    if (sid.isEmpty() || base.isEmpty()) return {};
    const QString path = hostDataUnder(base) + QStringLiteral("\\logs\\") + sid;
    return win::checkLockedDir(path, trustedSids()) == win::Check::Ok
               ? QDir::fromNativeSeparators(path)
               : QString();
}

} // namespace
#endif // Q_OS_WIN

#ifndef Q_OS_WIN
namespace {

/// The data directory, the directory the chain is checked from, and the uid
/// every directory on it must belong to.
struct Chain
{
    QString base;
    QString root;
    uid_t owner = 0;
};

Chain chain()
{
    const TestState test = testState();
    Chain c;
    if (test.active) {
        c.base = QDir::cleanPath(QFileInfo(test.installDir).absoluteFilePath());
        c.root = c.base;
        if (!test.trustedSids.isEmpty()) c.owner = uid_t(test.trustedSids.first().toUInt());
    } else {
        c.base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        c.root = QStringLiteral("/");
    }
    return c;
}

/// A directory open on @p fd belongs to @p owner, and neither its group nor
/// anyone else may write to it.
bool dirOk(int fd, uid_t owner)
{
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    return st.st_uid == owner && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

/// Each symbolic link on @p path, at or below @p root, belongs to @p owner.
bool linksOwned(const QString& path, const QString& root, uid_t owner)
{
    QString prefix = root == QLatin1String("/") ? QString() : root;
    if (path != prefix && !path.startsWith(prefix + QLatin1Char('/'))) return false;
    const QString rest = path.mid(prefix.size());
    for (const QString& part : rest.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        prefix += QLatin1Char('/') + part;
        struct stat st{};
        if (lstat(QFile::encodeName(prefix).constData(), &st) != 0) return false;
        if (S_ISLNK(st.st_mode) && st.st_uid != owner) return false;
    }
    return true;
}

/// @p dir checked, and with @p make, its missing directories made. Every
/// directory from the chain's root down to it is opened without following a
/// link and must pass dirOk(). Returns the directory's canonical path, empty
/// on a refusal.
QString walkChain(const QString& dir, bool make)
{
    const Chain c = chain();
    const QString target = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    const QString rootCanon = QFileInfo(c.root).canonicalFilePath();
    if (rootCanon.isEmpty()) return {};

    // The deepest directory that is there already; the rest is made below it.
    QString existing = target;
    QStringList missing;
    while (!QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent == existing) return {};
        missing.prepend(QFileInfo(existing).fileName());
        existing = parent;
    }
    if (!missing.isEmpty() && !make) return {};
    if (!linksOwned(existing, c.root, c.owner)) return {};

    const QString canon = QFileInfo(existing).canonicalFilePath();
    const bool under = rootCanon == QLatin1String("/") || canon == rootCanon ||
                       canon.startsWith(rootCanon + QLatin1Char('/'));
    if (canon.isEmpty() || !under) return {};

    int fd = ::open(QFile::encodeName(rootCanon).constData(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return {};
    auto next = [&](const QString& name, bool create) {
        const QByteArray n = QFile::encodeName(name);
        if (create && mkdirat(fd, n.constData(), 0700) != 0 && errno != EEXIST) return false;
        const int child =
            openat(fd, n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        fd = child;
        return fd >= 0 && dirOk(fd, c.owner);
    };
    bool ok = dirOk(fd, c.owner);
    const QString below = canon.mid(rootCanon == QLatin1String("/") ? 0 : rootCanon.size());
    for (const QString& part : below.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (ok) ok = next(part, false);
    for (const QString& part : missing)
        if (ok) ok = next(part, true);
    if (fd >= 0) ::close(fd);
    if (!ok) return {};

    QString out = canon;
    for (const QString& part : missing)
        out += QLatin1Char('/') + part;
    return out;
}

/// @p dir opened as itself, never through a link: -1 when it cannot be, or
/// (with @p systemOwned) when it does not belong to root or the TNR's
/// stand-in.
int openDirItself(const QString& dir, bool systemOwned)
{
    const int fd =
        ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || !systemOwned) return fd;
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_uid == chain().owner) return fd;
    ::close(fd);
    return -1;
}

QString posixPlace(const QString& leaf)
{
    const Chain c = chain();
    const QString dir = walkChain(c.base + QLatin1Char('/') + leaf, /*make=*/true);
    if (dir.isEmpty())
        say(QStringLiteral("%1 or a directory above it may be written by another account: no "
                           "file is written there")
                .arg(c.base + QLatin1Char('/') + leaf));
    return dir;
}

} // namespace
#endif // !Q_OS_WIN

namespace {

QString userDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

} // namespace

QString logDir(const QString& readerSid)
{
    if (!isPrivileged()) {
        const QString dir = userDataDir() + QStringLiteral("/logs");
        QDir().mkpath(dir);
        return dir;
    }
#ifdef Q_OS_WIN
    return privilegedLogDir(readerSid);
#else
    Q_UNUSED(readerSid);
    return posixPlace(QStringLiteral("logs"));
#endif
}

QString crashDir()
{
    if (!isPrivileged()) {
        const QString dir = userDataDir() + QStringLiteral("/crashes");
        QDir().mkpath(dir);
        return dir;
    }
#ifdef Q_OS_WIN
    return privilegedCrashDir();
#else
    return posixPlace(QStringLiteral("crashes"));
#endif
}

QString privateDir(const QString& name)
{
#ifdef Q_OS_WIN
    if (!isPrivileged() || !plainName(name) ||
        name.compare(QLatin1String("logs"), Qt::CaseInsensitive) == 0 ||
        name.compare(QLatin1String("crashes"), Qt::CaseInsensitive) == 0)
        return {};
    return privilegedPrivateDir(name);
#else
    Q_UNUSED(name);
    return {};
#endif
}

HeldFile::HeldFile(const QString& path)
{
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1 << 16, Qt::Uninitialized);
#ifdef Q_OS_WIN
    // Read access for others and nothing else: no write, no rename, no delete
    // while this handle is open.
    Handle h(CreateFileW(native(clean).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                         nullptr));
    if (!h.valid()) {
        m_Error = lastError();
        return;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.h, &info)) {
        m_Error = lastError();
        return;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1 || !win::samePath(win::finalPath(h.h), clean)) {
        m_Error = QStringLiteral("it is not a regular file with one link, at its own path");
        return;
    }
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h.h, buffer.data(), DWORD(buffer.size()), &got, nullptr)) {
            m_Error = lastError();
            return;
        }
        if (got == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), qsizetype(got)));
    }
    m_Handle = reinterpret_cast<qintptr>(h.release());
#else
    const int fd = ::open(QFile::encodeName(clean).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        m_Error = QString::fromLocal8Bit(strerror(errno));
        return;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        ::close(fd);
        m_Error = QStringLiteral("it is not a regular file with one link");
        return;
    }
    for (;;) {
        const ssize_t got = ::read(fd, buffer.data(), size_t(buffer.size()));
        if (got < 0) {
            m_Error = QString::fromLocal8Bit(strerror(errno));
            ::close(fd);
            return;
        }
        if (got == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), qsizetype(got)));
    }
    m_Handle = fd;
#endif
    m_Sha256 = QString::fromLatin1(hash.result().toHex());
    m_Held = true;
}

HeldFile::~HeldFile()
{
    release();
}

void HeldFile::release()
{
    if (!m_Held) return;
    m_Held = false;
#ifdef Q_OS_WIN
    CloseHandle(reinterpret_cast<HANDLE>(m_Handle));
#else
    ::close(int(m_Handle));
#endif
    m_Handle = -1;
}

std::optional<QByteArray> readInstalledFile(const QString& path, qint64 maxBytes, QString* why,
                                            bool* missing, QDateTime* modified)
{
    QString reason;
    bool absent = false;
    auto refuse = [&](const QString& text) -> std::optional<QByteArray> {
        if (why) *why = text;
        if (missing) *missing = absent;
        return std::nullopt;
    };
    if (missing) *missing = false;
    const QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    const QString clean = info.absoluteFilePath();
#ifdef Q_OS_WIN
    // Opened as itself, and read through the handle the checks were made on.
    Handle h(CreateFileW(native(clean).c_str(), GENERIC_READ | READ_CONTROL,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) {
        const DWORD err = GetLastError();
        absent = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        return refuse(lastError(err));
    }
    const QStringList trusted = trustedSids();
    QString resolved;
    const win::Check dir = win::checkInstallDir(info.absolutePath(), trusted, &resolved);
    if (dir != win::Check::Ok || !win::samePath(resolved, info.absolutePath()))
        return refuse(
            QStringLiteral("its directory is %1")
                .arg(win::describe(dir != win::Check::Ok ? dir : win::Check::ReparsePoint)));
    BY_HANDLE_FILE_INFORMATION facts{};
    if (!GetFileInformationByHandle(h.h, &facts)) return refuse(lastError());
    if ((facts.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        facts.nNumberOfLinks != 1 || !win::samePath(win::finalPath(h.h), clean))
        return refuse(QStringLiteral("it is not a regular file with one link, at its own path"));
    QStringList owners = trusted;
    owners << QStringLiteral("S-1-5-18") << QStringLiteral("S-1-5-32-544")
           << QString::fromWCharArray(win::kTrustedInstaller);
    const win::Check file = win::checkSecurity(h.h, win::SidSet(owners), /*locked=*/false);
    if (file != win::Check::Ok) return refuse(win::describe(file));
    const qint64 size = (qint64(facts.nFileSizeHigh) << 32) | qint64(facts.nFileSizeLow);
    if (size > maxBytes) return refuse(QStringLiteral("it holds more than %1 bytes").arg(maxBytes));

    QByteArray data;
    QByteArray buffer(1 << 16, Qt::Uninitialized);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h.h, buffer.data(), DWORD(buffer.size()), &got, nullptr))
            return refuse(lastError());
        if (got == 0) break;
        data.append(buffer.constData(), qsizetype(got));
        if (data.size() > maxBytes)
            return refuse(QStringLiteral("it holds more than %1 bytes").arg(maxBytes));
    }
    if (modified) {
        // A FILETIME counts 100 ns from 1601; Qt counts milliseconds from 1970.
        const quint64 ticks = (quint64(facts.ftLastWriteTime.dwHighDateTime) << 32) |
                              quint64(facts.ftLastWriteTime.dwLowDateTime);
        *modified = QDateTime::fromMSecsSinceEpoch(qint64(ticks / 10000) - 11644473600000LL);
    }
    return data;
#else
    QFile file(clean);
    if (!file.open(QIODevice::ReadOnly)) {
        absent = !info.exists();
        return refuse(file.errorString());
    }
    if (file.size() > maxBytes)
        return refuse(QStringLiteral("it holds more than %1 bytes").arg(maxBytes));
    const QByteArray data = file.read(maxBytes + 1);
    if (data.size() > maxBytes)
        return refuse(QStringLiteral("it holds more than %1 bytes").arg(maxBytes));
    if (modified) *modified = file.fileTime(QFileDevice::FileModificationTime);
    return data;
#endif
}

QStringList readableLogDirs()
{
    if (isPrivileged()) {
        const QString dir = logDir();
        return dir.isEmpty() ? QStringList() : QStringList{dir};
    }
    QStringList dirs{userDataDir() + QStringLiteral("/logs")};
#ifdef Q_OS_WIN
    const QString hostData = ownHostDataLogs();
    if (!hostData.isEmpty()) dirs << hostData;
#endif
    return dirs;
}

bool isWritablePlace(const QString& dir)
{
    if (!isPrivileged()) return true;
#ifdef Q_OS_WIN
    return privilegedWritable(dir);
#else
    return !walkChain(dir, /*make=*/false).isEmpty();
#endif
}

bool openForAppend(QFile& file, QString* why)
{
    auto refuse = [why](const QString& reason) {
        if (why) *why = reason;
        return false;
    };
    if (!isPrivileged()) {
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return true;
        return refuse(file.errorString());
    }

    const QString path = QDir::cleanPath(QFileInfo(file.fileName()).absoluteFilePath());
    const QString dir = QFileInfo(path).absolutePath();
#ifdef Q_OS_WIN
    if (!privilegedWritable(dir))
        return refuse(QStringLiteral("its directory is not one made for this process's files"));

    // Opened as itself: a reparse point is seen, never followed. Append only.
    Handle h(CreateFileW(native(path).c_str(),
                         FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) return refuse(lastError());
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.h, &info)) return refuse(lastError());
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return refuse(QStringLiteral("it is a reparse point"));
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return refuse(QStringLiteral("it is a directory"));
    if (info.nNumberOfLinks != 1) return refuse(QStringLiteral("it has more than one link"));
    if (!win::samePath(win::finalPath(h.h), path))
        return refuse(QStringLiteral("it resolves outside its directory"));

    const int fd =
        _open_osfhandle(reinterpret_cast<intptr_t>(h.h), _O_WRONLY | _O_APPEND | _O_BINARY);
    if (fd == -1) return refuse(QStringLiteral("the C runtime refused its handle"));
    h.release(); // the descriptor owns it now
#else
    const QString canonDir = walkChain(dir, /*make=*/false);
    if (canonDir.isEmpty())
        return refuse(QStringLiteral("its directory, or one above it, may be written by another "
                                     "account"));
    const QByteArray name =
        QFile::encodeName(canonDir + QLatin1Char('/') + QFileInfo(path).fileName());
    const int fd =
        ::open(name.constData(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return refuse(QString::fromLocal8Bit(strerror(errno)));
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()) {
        ::close(fd);
        return refuse(QStringLiteral("it is not a regular file of this account's with one link"));
    }
#endif
    if (!file.open(fd, QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text,
                   QFileDevice::AutoCloseHandle)) {
#ifdef Q_OS_WIN
        _close(fd);
#else
        ::close(fd);
#endif
        return refuse(file.errorString());
    }
    return true;
}

SharedDir::SharedDir(const QString& dir, Owner owner, bool make)
    : m_Dir(QDir::cleanPath(QFileInfo(dir).absoluteFilePath()))
    , m_Privileged(isPrivileged())
{
    if (!m_Privileged) {
        m_Open = QFileInfo(m_Dir).isDir() || (make && QDir().mkpath(m_Dir));
        if (!m_Open) m_Error = QStringLiteral("%1 is not a directory").arg(m_Dir);
        return;
    }
    const bool systemOwned = owner == Owner::System;
#ifdef Q_OS_WIN
    bool missing = false;
    HANDLE h = openDirItself(m_Dir, systemOwned, &m_Error, &missing);
    if (h == INVALID_HANDLE_VALUE && missing && make) {
        // Made with what the directory above hands down, then opened and
        // checked like one that was there.
        if (!CreateDirectoryW(native(m_Dir).c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            m_Error = QStringLiteral("the directory cannot be made: %1").arg(lastError());
            return;
        }
        h = openDirItself(m_Dir, systemOwned, &m_Error, &missing);
    }
    if (h == INVALID_HANDLE_VALUE) return;
    m_Handle = reinterpret_cast<qintptr>(h);
#else
    int fd = openDirItself(m_Dir, systemOwned);
    if (fd < 0 && make && errno == ENOENT &&
        ::mkdir(QFile::encodeName(m_Dir).constData(), 0755) == 0)
        fd = openDirItself(m_Dir, systemOwned);
    if (fd < 0) {
        m_Error = QStringLiteral("the directory cannot be opened as itself, or belongs to "
                                 "another account");
        return;
    }
    m_Handle = fd;
#endif
    m_Error.clear();
    m_Open = true;
}

SharedDir::~SharedDir()
{
    if (!m_Privileged || !m_Open) return;
#ifdef Q_OS_WIN
    CloseHandle(reinterpret_cast<HANDLE>(m_Handle));
#else
    ::close(int(m_Handle));
#endif
}

bool SharedDir::open(QFile& file, const QString& name, QIODevice::OpenMode mode, QString* why,
                     bool* missing) const
{
    QString reason;
    bool absent = false;
    auto refuse = [&](const QString& text) {
        if (why) *why = text.isEmpty() ? reason : text;
        if (missing) *missing = absent;
        return false;
    };
    if (missing) *missing = false;
    if (!m_Open) return refuse(m_Error);
    file.setFileName(m_Dir + QLatin1Char('/') + name);
    const bool write = mode & QIODevice::WriteOnly;
    const QIODevice::OpenMode text = mode & QIODevice::Text;
    if (!m_Privileged) {
        absent = !write && !QFileInfo::exists(file.fileName());
        const QIODevice::OpenMode plain =
            write ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::ReadOnly;
        if (file.open(plain | text)) return true;
        return refuse(file.errorString());
    }

#ifdef Q_OS_WIN
    Handle h(openIn(reinterpret_cast<HANDLE>(m_Handle), name, write ? Shared::Write : Shared::Read,
                    &reason, &absent));
    if (!h.valid()) return refuse({});
    if (write) {
        // Emptied through the handle that was checked, and only now.
        FILE_END_OF_FILE_INFO end{};
        if (!SetFileInformationByHandle(h.h, FileEndOfFileInfo, &end, sizeof(end)))
            return refuse(lastError());
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h.h),
                                   (write ? _O_WRONLY : _O_RDONLY) | _O_BINARY);
    if (fd == -1) return refuse(QStringLiteral("the C runtime refused its handle"));
    h.release(); // the descriptor owns it now
#else
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".") ||
        name == QLatin1String(".."))
        return refuse(QStringLiteral("it cannot be opened by its name in its directory"));
    // O_NONBLOCK for the open alone: a name that is a FIFO would otherwise
    // hold this process until someone opened its other end.
    const int fd =
        openat(int(m_Handle), QFile::encodeName(name).constData(),
               (write ? O_WRONLY | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0644);
    if (fd < 0) {
        absent = errno == ENOENT;
        return refuse(QString::fromLocal8Bit(strerror(errno)));
    }
    struct stat st{};
    const int flags = fcntl(fd, F_GETFL);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || flags == -1 ||
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1 || (write && ftruncate(fd, 0) != 0)) {
        ::close(fd);
        return refuse(QStringLiteral("it is not a regular file with one link"));
    }
#endif
    if (!file.open(fd, (write ? QIODevice::WriteOnly : QIODevice::ReadOnly) | text,
                   QFileDevice::AutoCloseHandle)) {
#ifdef Q_OS_WIN
        _close(fd);
#else
        ::close(fd);
#endif
        return refuse(file.errorString());
    }
    return true;
}

std::optional<QByteArray> SharedDir::read(const QString& name, qint64 maxBytes, QString* why,
                                          bool* missing) const
{
    QFile file;
    if (!open(file, name, QIODevice::ReadOnly, why, missing)) return std::nullopt;
    if (file.size() > maxBytes) {
        if (why) *why = QStringLiteral("it holds more than %1 bytes").arg(maxBytes);
        return std::nullopt;
    }
    // One more byte than allowed would show a file that grew since.
    const QByteArray data = file.read(maxBytes + 1);
    if (data.size() > maxBytes) {
        if (why) *why = QStringLiteral("it holds more than %1 bytes").arg(maxBytes);
        return std::nullopt;
    }
    return data;
}

bool SharedDir::write(const QString& name, const QByteArray& data, QString* why) const
{
    QFile file;
    if (!open(file, name, QIODevice::WriteOnly, why)) return false;
    const bool ok = file.write(data) == data.size() && file.flush();
    if (!ok && why) *why = file.errorString();
    file.close();
    return ok;
}

bool SharedDir::remove(const QString& name) const
{
    if (!m_Open) return false;
    const QString path = m_Dir + QLatin1Char('/') + name;
    if (!m_Privileged) return QFile::remove(path) || !QFileInfo::exists(path);
#ifdef Q_OS_WIN
    QString why;
    bool missing = false;
    Handle h(openIn(reinterpret_cast<HANDLE>(m_Handle), name, Shared::Delete, &why, &missing));
    if (!h.valid()) return missing;
    return deleteByHandle(h.h);
#else
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".") ||
        name == QLatin1String(".."))
        return false;
    // unlinkat removes the name itself, a link included, and no directory.
    return unlinkat(int(m_Handle), QFile::encodeName(name).constData(), 0) == 0 || errno == ENOENT;
#endif
}

bool openShared(QFile& file, QIODevice::OpenMode mode, QString* why)
{
    const QFileInfo info(file.fileName());
    return SharedDir(info.absolutePath()).open(file, info.fileName(), mode, why);
}

bool removeShared(const QString& path)
{
    const QFileInfo info(path);
    return SharedDir(info.absolutePath()).remove(info.fileName());
}

QString traceDir(const QString& logFilePath)
{
    if (!logFilePath.isEmpty()) {
        const QString dir = QFileInfo(logFilePath).absolutePath();
        return isWritablePlace(dir) ? dir : QString();
    }
    return isPrivileged() ? QString() : QDir::tempPath();
}

void pruneWorkerFiles(const QString& dir, int maxAgeDays)
{
    if (dir.isEmpty() || !isWritablePlace(dir)) return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    const QStringList patterns{
        QStringLiteral("moonlightweb-worker-*.log"), QStringLiteral("click-trace-*.csv"),
        QStringLiteral("relay-frames-*.csv"), QStringLiteral("relay-audio-*.csv")};
    const QFileInfoList files =
        QDir(dir).entryInfoList(patterns, QDir::Files | QDir::NoSymLinks | QDir::Hidden);
    for (const QFileInfo& f : files)
        if (f.lastModified() < cutoff) QFile::remove(f.absoluteFilePath());
}

int removeHostData(bool withLogs)
{
#ifdef Q_OS_WIN
    // Only under an install directory that passes its check and is its own
    // path: no privileged process made a host-data anywhere else, so nothing
    // is deleted there.
    const QString requested = installRequested();
    QString base;
    const win::Check check = win::checkInstallDir(requested, trustedSids(), &base);
    if (check != win::Check::Ok || !win::samePath(base, requested)) {
        say(QStringLiteral("%1 is %2: nothing is deleted under it")
                .arg(QDir::toNativeSeparators(requested),
                     check != win::Check::Ok ? win::describe(check)
                                             : win::describe(win::Check::ReparsePoint)));
        return 0;
    }
    const QString hostData = hostDataUnder(base);
    // With the logs: all of it. Without: everything but them (the crash dumps,
    // what was staged to be run).
    QStringList targets{hostData};
    if (!withLogs) {
        targets.clear();
        const QStringList names = QDir(hostData).entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                           QDir::Hidden | QDir::System);
        for (const QString& name : names)
            if (name.compare(QLatin1String("logs"), Qt::CaseInsensitive) != 0)
                targets << hostData + QLatin1Char('\\') + name;
    }
    for (const QString& target : targets) {
        if (!removeNoFollow(target)) {
            say(QStringLiteral("cannot delete %1: %2").arg(target, lastError()));
            return 1;
        }
    }
    if (!withLogs) {
        // What is left empty goes as well. RemoveDirectoryW takes a directory
        // (or a junction, as itself) that holds nothing, and nothing else.
        RemoveDirectoryW(native(hostData + QStringLiteral("\\logs")).c_str());
        RemoveDirectoryW(native(hostData).c_str());
    }
    return 0;
#else
    Q_UNUSED(withLogs);
    return 0;
#endif
}

} // namespace mw::place
