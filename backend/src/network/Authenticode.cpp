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

#include "network/Authenticode.h"

#include <QByteArray>
#include <QDir>

#include <string>

// clang-format off
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
// clang-format on

namespace Authenticode {

// The subject of the certificate that signed this PE file, read straight from
// its embedded signature. No trust decision here — this is only a name, used
// to compare the installed exe's signer with the new installer's.
QString signerSubject(const QString& path)
{
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                          QDir::toNativeSeparators(path).toStdWString().c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY,
                          0, nullptr, nullptr, nullptr, &store, &msg, nullptr))
        return {};

    QString subject;
    DWORD len = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &len) && len > 0) {
        QByteArray buf(int(len), Qt::Uninitialized);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, buf.data(), &len)) {
            auto* info = reinterpret_cast<PCERT_INFO>(buf.data());
            if (PCCERT_CONTEXT cert =
                    CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                               CERT_FIND_SUBJECT_CERT, info, nullptr)) {
                const DWORD n = CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject,
                                               CERT_X500_NAME_STR, nullptr, 0);
                std::wstring s(n, L'\0');
                CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR,
                               s.data(), n);
                subject = QString::fromWCharArray(s.c_str());
                CertFreeCertificateContext(cert);
            }
        }
    }
    CryptMsgClose(msg);
    CertCloseStore(store, 0);
    return subject;
}

// Authenticode, with chains built from the machine's stores only: a root the
// user added to their own store (which needs no elevation) proves nothing here.
long verify(const QString& path)
{
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = w.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags =
        WTD_USE_LOCAL_MACHINE_CERTS | WTD_REVOCATION_CHECK_NONE | WTD_DISABLE_MD2_MD4;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return result;
}

} // namespace Authenticode
