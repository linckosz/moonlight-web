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

#pragma once

#include <QJsonObject>
#include <QString>

/**
 * The pure half of `--update-apply` (UpdateApply.h): what the elevated helper
 * accepts to run, decided from GitHub's answer and nothing else.
 *
 * Every input here comes from the binary itself (product name, version,
 * architecture) or from api.github.com over a TLS chain checked against the
 * machine's roots. Nothing the unelevated user can write is consulted: the
 * staged file is only ever the thing being judged.
 */
namespace UpdateApplyRules {

/// The tail of the Windows installer's asset name (release.yml):
/// "win-x64.exe" or "win-arm64.exe".
QString windowsAssetSuffix(bool arm64);

struct Installer
{
    QString version; ///< "0.4.1", the tag without its 'v'
    QString name;    ///< MoonlightWeb-installer-0.4.1-win-x64.exe
    qint64 size = 0;
    QString sha256; ///< 64 lowercase hex digits
};

/// What the staged installer has to be. Refuses (non-empty error, *out left
/// alone) a draft or a pre-release, a version not strictly newer than
/// `currentVersion` (no downgrade), a release with no single asset named
/// "<productName>-installer-*<assetSuffix>", and an asset without a size or a
/// sha256 digest: in doubt, nothing runs.
QString expectedInstaller(const QJsonObject& release, const QString& productName,
                          const QString& currentVersion, const QString& assetSuffix,
                          Installer* out);

/// Whether a file of `size` bytes hashing to `sha256Hex` is that installer.
/// Empty when it is, the reason otherwise.
QString matches(const Installer& expected, qint64 size, const QString& sha256Hex);

/// Step 4: whether the staged installer may run, by who signed what.
enum class Signer
{
    MayRun,      ///< this install is unsigned (nothing to compare with), or the same signer
    NotVerified, ///< this install is signed, the installer's Authenticode does not verify
    OtherSigner, ///< both verify, but the installer is signed by someone else
};

/// `installedSigner` is the subject that signed the running exe (empty when it
/// is unsigned), `verified` whether the installer's Authenticode verified, and
/// `installerSigner` the subject that signed the installer. An unsigned install
/// runs whatever passed the digest: the digest is then the whole proof.
Signer signerVerdict(const QString& installedSigner, bool verified, const QString& installerSigner);

} // namespace UpdateApplyRules
