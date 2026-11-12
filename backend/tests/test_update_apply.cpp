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

#include "common/InheritedEnvironment.h"
#include "network/UpdateApplyRules.h"
#include "test_framework.h"

#include <QJsonArray>
#include <QJsonObject>

// The pure half of the elevated update helper: which installer it accepts to
// run, decided from GitHub's answer alone, and which inherited variables every
// elevated helper drops. No network, no task, every platform.

namespace {

const QString kDigest =
    QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

QJsonObject asset(const QString& name, qint64 size = 60'000'000,
                  const QString& digest = QStringLiteral("sha256:") + kDigest)
{
    QJsonObject a;
    a["name"] = name;
    a["size"] = double(size);
    if (!digest.isNull()) a["digest"] = digest;
    return a;
}

QJsonObject release(const QString& tag, const QJsonArray& assets)
{
    QJsonObject r;
    r["tag_name"] = tag;
    r["draft"] = false;
    r["prerelease"] = false;
    r["assets"] = assets;
    return r;
}

const QString kX64 = QStringLiteral("win-x64.exe");
const QString kProduct = QStringLiteral("MoonlightWeb");

QJsonArray usual()
{
    return {asset(QStringLiteral("MoonlightWeb-installer-0.4.1-win-x64.exe")),
            asset(QStringLiteral("MoonlightWeb-installer-0.4.1-win-arm64.exe")),
            asset(QStringLiteral("moonlightweb-0.4.1-linux-x64.deb"))};
}

} // namespace

void run_update_apply_tests()
{
    using namespace UpdateApplyRules;

    SECTION("UpdateApply — the asset suffix per architecture, as release.yml names it");
    CHECK_EQ(windowsAssetSuffix(false), QStringLiteral("win-x64.exe"));
    CHECK_EQ(windowsAssetSuffix(true), QStringLiteral("win-arm64.exe"));

    SECTION("UpdateApply — a newer release's installer is what has to be staged");
    Installer want;
    CHECK(expectedInstaller(release(QStringLiteral("v0.4.1"), usual()), kProduct,
                            QStringLiteral("0.4.0"), kX64, &want)
              .isEmpty());
    CHECK_EQ(want.version, QStringLiteral("0.4.1"));
    CHECK_EQ(want.name, QStringLiteral("MoonlightWeb-installer-0.4.1-win-x64.exe"));
    CHECK_EQ(want.size, qint64(60'000'000));
    CHECK_EQ(want.sha256, kDigest);
    // The ARM64 one on an ARM64 machine, not the x64 one.
    Installer arm;
    CHECK(expectedInstaller(release(QStringLiteral("v0.4.1"), usual()), kProduct,
                            QStringLiteral("0.4.0"), windowsAssetSuffix(true), &arm)
              .isEmpty());
    CHECK_EQ(arm.name, QStringLiteral("MoonlightWeb-installer-0.4.1-win-arm64.exe"));
    // A staging build's forced version updates to the same latest release.
    Installer staging;
    CHECK(expectedInstaller(release(QStringLiteral("v0.4.1"), usual()), kProduct,
                            QStringLiteral("0.3.0.g7bb-stg"), kX64, &staging)
              .isEmpty());

    SECTION("UpdateApply — never the same version, never an older one");
    Installer none;
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"), usual()), kProduct,
                             QStringLiteral("0.4.1"), kX64, &none)
               .isEmpty());
    CHECK(!expectedInstaller(release(QStringLiteral("v0.3.1"), usual()), kProduct,
                             QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());
    // A source build has no version of its own: nothing is newer than it.
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"), usual()), kProduct,
                             QStringLiteral("0.0.0-dev"), kX64, &none)
               .isEmpty());
    CHECK(none.sha256.isEmpty()); // left alone on every refusal

    SECTION("UpdateApply — drafts, pre-releases and untagged answers are refused");
    QJsonObject draft = release(QStringLiteral("v0.4.1"), usual());
    draft["draft"] = true;
    CHECK(!expectedInstaller(draft, kProduct, QStringLiteral("0.4.0"), kX64, &none).isEmpty());
    QJsonObject pre = release(QStringLiteral("v0.4.1"), usual());
    pre["prerelease"] = true;
    CHECK(!expectedInstaller(pre, kProduct, QStringLiteral("0.4.0"), kX64, &none).isEmpty());
    CHECK(!expectedInstaller(release(QString(), usual()), kProduct, QStringLiteral("0.4.0"), kX64,
                             &none)
               .isEmpty());

    SECTION("UpdateApply — exactly one asset of this edition, or nothing runs");
    // No Windows installer at all.
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"),
                                     {asset(QStringLiteral("moonlightweb-0.4.1-linux-x64.deb"))}),
                             kProduct, QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());
    // A DEV installer never stands in for the production one…
    const QJsonArray devOnly{asset(QStringLiteral("MoonlightWebDev-installer-0.4.1-win-x64.exe"))};
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"), devOnly), kProduct,
                             QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());
    // …and does not disturb it when both are attached.
    QJsonArray both = usual();
    both.append(asset(QStringLiteral("MoonlightWebDev-installer-0.4.1-win-x64.exe")));
    Installer prod;
    CHECK(expectedInstaller(release(QStringLiteral("v0.4.1"), both), kProduct,
                            QStringLiteral("0.4.0"), kX64, &prod)
              .isEmpty());
    CHECK_EQ(prod.name, QStringLiteral("MoonlightWeb-installer-0.4.1-win-x64.exe"));
    // Two candidates: which one the server downloaded would be a guess.
    QJsonArray twice = usual();
    twice.append(asset(QStringLiteral("MoonlightWeb-installer-0.4.1-signed-win-x64.exe")));
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"), twice), kProduct,
                             QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());

    SECTION("UpdateApply — no size or no sha256 digest from GitHub, no install");
    const QString name = QStringLiteral("MoonlightWeb-installer-0.4.1-win-x64.exe");
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"), {asset(name, 0)}), kProduct,
                             QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());
    CHECK(!expectedInstaller(release(QStringLiteral("v0.4.1"), {asset(name, 1000, QString())}),
                             kProduct, QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());
    CHECK(
        !expectedInstaller(release(QStringLiteral("v0.4.1"),
                                   {asset(name, 1000, QStringLiteral("sha1:") + kDigest.left(40))}),
                           kProduct, QStringLiteral("0.4.0"), kX64, &none)
             .isEmpty());
    CHECK(!expectedInstaller(
               release(QStringLiteral("v0.4.1"),
                       {asset(name, 1000, QStringLiteral("sha256:") + kDigest.left(63))}),
               kProduct, QStringLiteral("0.4.0"), kX64, &none)
               .isEmpty());
    // Upper-case hex from GitHub is the same digest.
    Installer upper;
    CHECK(expectedInstaller(
              release(QStringLiteral("v0.4.1"),
                      {asset(name, 1000, QStringLiteral("sha256:") + kDigest.toUpper())}),
              kProduct, QStringLiteral("0.4.0"), kX64, &upper)
              .isEmpty());
    CHECK_EQ(upper.sha256, kDigest);

    SECTION("UpdateApply — the staged bytes have to be GitHub's, size and digest");
    CHECK(matches(want, want.size, kDigest).isEmpty());
    CHECK(matches(want, want.size, kDigest.toUpper()).isEmpty());
    CHECK(!matches(want, want.size - 1, kDigest).isEmpty());
    QString other = kDigest;
    other[0] = QLatin1Char('f');
    CHECK(!matches(want, want.size, other).isEmpty());
    CHECK(!matches(want, want.size, QString()).isEmpty()); // unreadable copy
    CHECK(!matches(Installer{}, 0, QString()).isEmpty());  // nothing to compare with

    SECTION("InheritedEnvironment — what loads code goes, the rest stays");
    using mw::inheritedenv::isCodeLoading;
    for (const char* n :
         {"QT_PLUGIN_PATH", "qt_plugin_path", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH",
          "QML2_IMPORT_PATH", "OPENSSL_CONF", "OPENSSL_MODULES", "OPENSSL_ENGINES", "openssl_conf",
          "SSL_CERT_FILE", "SSL_CERT_DIR", "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_LAYER_PATH",
          "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS"})
        CHECK(isCodeLoading(QString::fromLatin1(n)));
    for (const char* n :
         {"PATH", "TEMP", "LOCALAPPDATA", "USERPROFILE", "SESSIONNAME", "MW_SERVICE", "MW_DEBUG",
          "QT_LOGGING_RULES", "QT_QPA_PLATFORM", "SSL_KEYLOG", "VKX", "OPENSSL"})
        CHECK(!isCodeLoading(QString::fromLatin1(n)));

    SECTION("InheritedEnvironment — the elevated modes are recognised, nothing else");
    {
        char exe[] = "MoonlightWeb.exe";
        char update[] = "--update-apply";
        char server[] = "--autostart";
        char* elevated[] = {exe, update};
        char* plain[] = {exe, server};
        char* lone[] = {exe};
        CHECK(mw::inheritedenv::isElevatedHelper(2, elevated));
        CHECK(!mw::inheritedenv::isElevatedHelper(2, plain));
        CHECK(!mw::inheritedenv::isElevatedHelper(1, lone));
        // argv[0] is a path, never a mode.
        char* asExe[] = {update};
        CHECK(!mw::inheritedenv::isElevatedHelper(1, asExe));

        // The other elevated modes, in both spellings, wherever they sit.
        char vdisplay[] = "--vdisplay-apply";
        char stage[] = "--stage=driver";
        char vhid[] = "--vhid-apply";
        char vhidEq[] = "--vhid-apply=install";
        char verb[] = "install";
        char worker[] = "--stream-worker";
        char pipe[] = "--worker-pipe";
        char* vd[] = {exe, stage, vdisplay};
        char* vh[] = {exe, vhid, verb};
        char* vhe[] = {exe, vhidEq};
        char* sw[] = {exe, worker, pipe};
        CHECK(mw::inheritedenv::isElevatedHelper(3, vd));
        CHECK(mw::inheritedenv::isElevatedHelper(3, vh));
        CHECK(mw::inheritedenv::isElevatedHelper(2, vhe));
        CHECK(mw::inheritedenv::isElevatedHelper(3, sw));
        // A longer flag that merely starts like one is not that mode.
        char lookalike[] = "--stream-worker-x";
        char serviceFlag[] = "--worker-service";
        char* la[] = {exe, lookalike};
        char* ws[] = {exe, serviceFlag};
        CHECK(!mw::inheritedenv::isElevatedHelper(2, la));
        CHECK(!mw::inheritedenv::isElevatedHelper(2, ws));
    }

    SECTION("InheritedEnvironment — an elevated start is cleaned whatever it was asked to do");
    {
        using mw::inheritedenv::mustScrub;
        // An elevated mode, by its name: cleaned even when the token says
        // nothing (it could not be read).
        CHECK(mustScrub(true, false));
        // Any other start that runs elevated: a command the installer runs,
        // the server started by hand as an administrator.
        CHECK(mustScrub(false, true));
        CHECK(mustScrub(true, true));
        // An ordinary start keeps its environment as it is.
        CHECK(!mustScrub(false, false));

        // A command outside the list is cleaned through its token alone.
        char exe[] = "MoonlightWeb.exe";
        char wait[] = "--wait-link";
        char* installerCommand[] = {exe, wait};
        const bool byName = mw::inheritedenv::isElevatedHelper(2, installerCommand);
        CHECK(!byName);
        CHECK(mustScrub(byName, true));
        CHECK(!mustScrub(byName, false));
    }
}
