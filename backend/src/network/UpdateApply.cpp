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

#include "network/UpdateApply.h"

#include "common/Edition.h"
#include "common/Logger.h"
#include "common/PrivilegedPlace.h"

#ifdef Q_OS_WIN
#include "network/AddressRace.h"
#include "network/Authenticode.h"
#include "network/SelfUpdater.h"
#include "network/UpdateApplyRules.h"
#include "network/UpdateChecker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <string>

// clang-format off
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
// clang-format on
#endif

namespace UpdateApply {

#ifdef Q_OS_WIN
namespace {

const char* const kTag = "[UpdateApply]";

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

// The directory the installer is copied to, judged and run from. Created anew
// on every run, by us: CreateDirectoryW failing on an existing entry is what
// guarantees the directory is the one carrying our DACL, and not something
// planted (or re-planted) between the delete and the create.
QString makeLockedDir(QString* error)
{
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/update-staging");
    // Deleted without following a reparse point (common/PrivilegedPlace.h).
    if (!mw::place::removeNoFollow(dir)) {
        *error =
            QStringLiteral("cannot clear %1: %2").arg(QDir::toNativeSeparators(dir), lastError());
        return {};
    }
    // Owned by Administrators, protected (no inheritance from {app}), full
    // control for SYSTEM and Administrators, nothing at all for anyone else —
    // not even read: the copy is made with our rights, so a staged "file" that
    // is really a link to something only an administrator may read must not
    // become readable here.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
        *error = QStringLiteral("cannot build the directory's DACL: %1").arg(lastError());
        return {};
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const BOOL created = CreateDirectoryW(native(dir).c_str(), &sa);
    const DWORD err = GetLastError();
    LocalFree(sd);
    if (!created) {
        *error = QStringLiteral("cannot create %1: %2")
                     .arg(QDir::toNativeSeparators(dir), lastError(err));
        return {};
    }
    return dir;
}

// The chain the TLS connection presented, judged again by the machine's chain
// engine (HCCE_LOCAL_MACHINE): Qt has already accepted it, but with the user's
// roots in the set, and the digest it carries is what decides what runs.
QString verifyServerChain(const QList<QSslCertificate>& chain, const QString& host)
{
    if (chain.isEmpty()) return QStringLiteral("the TLS peer presented no certificate");
    HCERTSTORE extra =
        CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!extra) return QStringLiteral("cannot open a memory store: %1").arg(lastError());
    PCCERT_CONTEXT leaf = nullptr;
    for (int i = 0; i < chain.size(); ++i) {
        const QByteArray der = chain[i].toDer();
        CertAddEncodedCertificateToStore(
            extra, X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(der.constData()),
            DWORD(der.size()), CERT_STORE_ADD_ALWAYS, i == 0 ? &leaf : nullptr);
    }
    QString error;
    if (!leaf) {
        error = QStringLiteral("cannot decode the server certificate");
    } else {
        LPSTR serverAuth[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        CERT_CHAIN_PARA para{};
        para.cbSize = sizeof(para);
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        para.RequestedUsage.Usage.cUsageIdentifier = 1;
        para.RequestedUsage.Usage.rgpszUsageIdentifier = serverAuth;
        PCCERT_CHAIN_CONTEXT ctx = nullptr;
        if (!CertGetCertificateChain(HCCE_LOCAL_MACHINE, leaf, nullptr, extra, &para, 0, nullptr,
                                     &ctx)) {
            error = QStringLiteral("cannot build the server's chain: %1").arg(lastError());
        } else {
            std::wstring name = host.toStdWString();
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
            ssl.cbSize = sizeof(ssl);
            ssl.dwAuthType = AUTHTYPE_SERVER;
            ssl.pwszServerName = name.data();
            CERT_CHAIN_POLICY_PARA policy{};
            policy.cbSize = sizeof(policy);
            policy.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS status{};
            status.cbSize = sizeof(status);
            if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, ctx, &policy, &status))
                error = QStringLiteral("cannot check the server's chain: %1").arg(lastError());
            else if (status.dwError != 0)
                error = QStringLiteral("the machine's roots do not vouch for %1: %2")
                            .arg(host, lastError(status.dwError));
            CertFreeCertificateChain(ctx);
        }
        CertFreeCertificateContext(leaf);
    }
    CertCloseStore(extra, 0);
    return error;
}

// GitHub's answer for the latest release, from the URL compiled in. No relay,
// no redirect, no proxy: the bytes have to come from api.github.com itself.
QString fetchLatestRelease(QJsonObject* release)
{
    const QUrl url(UpdateChecker::releasesApiUrl());
    QNetworkAccessManager nam;
    nam.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "MoonlightWeb-UpdateApply");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    AddressRace::enableOn(req);
    req.setTransferTimeout(30000);

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
        return QStringLiteral("GitHub: %1").arg(reply->errorString());
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) return QStringLiteral("GitHub answered HTTP %1").arg(status);
    if (const QString bad =
            verifyServerChain(reply->sslConfiguration().peerCertificateChain(), url.host());
        !bad.isEmpty())
        return bad;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) return QStringLiteral("GitHub's answer is not a JSON object");
    *release = doc.object();
    return {};
}

// Byte for byte into a file created here, rather than CopyFile: the copy has
// to carry the locked directory's DACL and nothing of the source's (streams,
// attributes, security), and must not replace anything already there.
QString copyInto(const QString& source, const QString& target)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly))
        return QStringLiteral("cannot read %1: %2")
            .arg(QDir::toNativeSeparators(source), in.errorString());
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return QStringLiteral("cannot create %1: %2")
            .arg(QDir::toNativeSeparators(target), out.errorString());
    QByteArray chunk;
    while (!(chunk = in.read(1 << 20)).isEmpty())
        if (out.write(chunk) != chunk.size())
            return QStringLiteral("cannot write %1: %2")
                .arg(QDir::toNativeSeparators(target), out.errorString());
    if (in.error() != QFileDevice::NoError)
        return QStringLiteral("cannot read %1: %2")
            .arg(QDir::toNativeSeparators(source), in.errorString());
    return {};
}

QString sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

QString knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    QString out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)))
        out = QString::fromWCharArray(path);
    CoTaskMemFree(path);
    return out;
}

// The installer's environment, built rather than inherited. Ours came from
// HKCU\Environment, which the unelevated user writes; the installer runs
// other programs elevated and extracts its own helpers to %TEMP%. Paths come
// from the system and the shell's known folders, TEMP is the locked
// directory, and only names that load nothing are copied over.
std::wstring installerEnvironment(const QString& tempDir)
{
    QList<QPair<QString, QString>> vars;
    wchar_t buf[MAX_PATH];
    const QString windows = GetSystemWindowsDirectoryW(buf, MAX_PATH)
                                ? QString::fromWCharArray(buf)
                                : QStringLiteral("C:\\Windows");
    const QString system32 = GetSystemDirectoryW(buf, MAX_PATH)
                                 ? QString::fromWCharArray(buf)
                                 : windows + QStringLiteral("\\System32");
    vars << qMakePair(QStringLiteral("SystemRoot"), windows)
         << qMakePair(QStringLiteral("windir"), windows)
         << qMakePair(QStringLiteral("SystemDrive"), windows.left(2))
         << qMakePair(QStringLiteral("ComSpec"), system32 + QStringLiteral("\\cmd.exe"))
         << qMakePair(QStringLiteral("PATH"),
                      QStringList{system32, windows, system32 + QStringLiteral("\\Wbem"),
                                  system32 + QStringLiteral("\\WindowsPowerShell\\v1.0")}
                          .join(QLatin1Char(';')))
         << qMakePair(QStringLiteral("PATHEXT"),
                      QStringLiteral(".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"))
         << qMakePair(QStringLiteral("TEMP"), QDir::toNativeSeparators(tempDir))
         << qMakePair(QStringLiteral("TMP"), QDir::toNativeSeparators(tempDir));

    const struct
    {
        const char* name;
        const KNOWNFOLDERID& id;
    } folders[] = {
        {"USERPROFILE", FOLDERID_Profile},
        {"APPDATA", FOLDERID_RoamingAppData},
        {"LOCALAPPDATA", FOLDERID_LocalAppData},
        {"ProgramData", FOLDERID_ProgramData},
        {"ALLUSERSPROFILE", FOLDERID_ProgramData},
        {"PUBLIC", FOLDERID_Public},
        {"ProgramFiles", FOLDERID_ProgramFiles},
        {"ProgramFiles(x86)", FOLDERID_ProgramFilesX86},
        {"ProgramW6432", FOLDERID_ProgramFilesX64},
        {"CommonProgramFiles", FOLDERID_ProgramFilesCommon},
        {"CommonProgramFiles(x86)", FOLDERID_ProgramFilesCommonX86},
        {"CommonProgramW6432", FOLDERID_ProgramFilesCommonX64},
    };
    for (const auto& f : folders) {
        const QString path = knownFolder(f.id);
        if (!path.isEmpty()) vars << qMakePair(QString::fromLatin1(f.name), path);
    }
    const QString profile = knownFolder(FOLDERID_Profile);
    if (profile.size() > 2) {
        vars << qMakePair(QStringLiteral("HOMEDRIVE"), profile.left(2))
             << qMakePair(QStringLiteral("HOMEPATH"), profile.mid(2));
    }
    // Identity and hardware: read by installers, never a path to load from.
    for (const char* name :
         {"USERNAME", "USERDOMAIN", "USERDOMAIN_ROAMINGPROFILE", "USERDNSDOMAIN", "COMPUTERNAME",
          "LOGONSERVER", "SESSIONNAME", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE",
          "PROCESSOR_IDENTIFIER", "PROCESSOR_LEVEL", "PROCESSOR_REVISION", "OS"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty()) vars << qMakePair(QString::fromLatin1(name), value);
    }

    // CreateProcess wants the block sorted, case-insensitively, by name.
    std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    std::wstring block;
    for (const auto& [name, value] : vars) {
        block += (name + QLatin1Char('=') + value).toStdWString();
        block += L'\0';
    }
    block += L'\0';
    return block;
}

QString startInstaller(const QString& exe, const QString& dir)
{
    const QString tmp = dir + QStringLiteral("/tmp");
    if (!QDir().mkpath(tmp)) return QStringLiteral("cannot create the installer's TEMP");
    // Inno silent switches, as SelfUpdater passes them on the other paths, and
    // the log next to the installer: a directory the user cannot write, so an
    // elevated installer never writes through a link the user planted.
    std::wstring cmd = L"\"" + native(exe) +
                       L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- \"/LOG=" +
                       native(dir + QStringLiteral("/installer.log")) + L"\"";
    std::wstring env = installerEnvironment(tmp);
    const std::wstring cwd = native(dir);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(native(exe).c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.c_str(), &si, &pi))
        return QStringLiteral("cannot start the installer: %1").arg(lastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

int refuse(const QString& why)
{
    Logger::error(QStringLiteral("%1 refused: %2").arg(QLatin1String(kTag), why));
    return 1;
}

} // namespace

int run()
{
    // A DEV build is never offered an update (UpdateChecker::statusJson): the
    // only release there is installs the production edition beside it.
    if (mw::edition::isDevBuild())
        return refuse(QStringLiteral("a DEV build is not updated in place"));

    QString error;
    const QString dir = makeLockedDir(&error);
    if (dir.isEmpty()) return refuse(error);
    Logger::instance()->setLogFile(dir + QStringLiteral("/update-apply.log"), /*rotating=*/false);
    Logger::info(
        QStringLiteral("%1 version %2, %3")
            .arg(QLatin1String(kTag), mw::edition::version(), QSysInfo::currentCpuArchitecture()));

    // 1. The copy. Its source is whatever the user's profile holds — that is
    //    the point: from here on only the copy, in a directory the user cannot
    //    touch, is read, judged and run.
    const QString source =
        SelfUpdater::stagingDir() + QLatin1Char('/') + SelfUpdater::stagedFileName(QString());
    const QString copy = dir + QStringLiteral("/MoonlightWeb-update.exe");
    if (const QString bad = copyInto(source, copy); !bad.isEmpty()) return refuse(bad);

    // 2. What it has to be, from GitHub. Same architecture rule as the
    //    server's download (UpdateChecker::pickAsset): the CPU, not the build.
    QJsonObject release;
    if (const QString bad = fetchLatestRelease(&release); !bad.isEmpty()) return refuse(bad);
    const bool arm64 =
        QSysInfo::currentCpuArchitecture().contains(QLatin1String("arm"), Qt::CaseInsensitive);
    UpdateApplyRules::Installer expected;
    if (const QString bad = UpdateApplyRules::expectedInstaller(
            release, mw::edition::productName(), mw::edition::version(),
            UpdateApplyRules::windowsAssetSuffix(arm64), &expected);
        !bad.isEmpty())
        return refuse(bad);

    // 3. The bytes.
    if (const QString bad =
            UpdateApplyRules::matches(expected, QFileInfo(copy).size(), sha256Of(copy));
        !bad.isEmpty())
        return refuse(bad);

    // 4. The signer, for a signed install. An unsigned one (a fork, a CI run
    //    without the signing secrets) has nothing to compare with, and the
    //    digest above is the whole proof.
    const QString installedSigner =
        Authenticode::signerSubject(QCoreApplication::applicationFilePath());
    if (!installedSigner.isEmpty()) {
        const LONG trust = Authenticode::verify(copy);
        const QString signer =
            trust == ERROR_SUCCESS ? Authenticode::signerSubject(copy) : QString();
        switch (UpdateApplyRules::signerVerdict(installedSigner, trust == ERROR_SUCCESS, signer)) {
        case UpdateApplyRules::Signer::MayRun: break;
        case UpdateApplyRules::Signer::NotVerified:
            return refuse(QStringLiteral("the installer's signature does not verify: %1")
                              .arg(lastError(DWORD(trust))));
        case UpdateApplyRules::Signer::OtherSigner:
            return refuse(
                QStringLiteral("the installer is signed by \"%1\", this install by \"%2\"")
                    .arg(signer, installedSigner));
        }
    }

    // 5. Run it, and get out of its way.
    if (const QString bad = startInstaller(copy, dir); !bad.isEmpty()) return refuse(bad);
    Logger::info(
        QStringLiteral("%1 installer %2 (%3, sha256 %4) started%5")
            .arg(QLatin1String(kTag), expected.name, expected.version, expected.sha256.left(12),
                 installedSigner.isEmpty() ? QStringLiteral(", unsigned build")
                                           : QStringLiteral(", signer ") + installedSigner));
    return 0;
}

#else

int run()
{
    Logger::error(QStringLiteral("[UpdateApply] --update-apply exists on Windows only"));
    return 1;
}

#endif

} // namespace UpdateApply
