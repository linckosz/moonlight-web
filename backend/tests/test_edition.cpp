/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/Edition.h"

using namespace mw::edition;

// The names a build answers to decide where its state lives and what the
// operating system recognises it by. A DEV build that came out under the
// production name would upgrade over the production install and share its
// pairings — the bug this edition exists to prevent — so every combination is
// pinned here.
void run_edition_tests()
{
    SECTION("Edition");

    // State: three separate homes. A plain --dev run keeps the name every
    // existing dev instance already stores its pairings under.
    CHECK_EQ(dataNameFor(false, false), QStringLiteral("MoonlightWeb"));
    CHECK_EQ(dataNameFor(true, false), QStringLiteral("MoonlightWebDev"));
    CHECK_EQ(dataNameFor(false, true), QStringLiteral("MoonlightWeb-dev"));
    CHECK_EQ(dataNameFor(true, true), QStringLiteral("MoonlightWebDev-dev"));

    // What a person reads: production, or DEV — whichever way it became DEV.
    CHECK_EQ(displayNameFor(false, false), QStringLiteral("MoonlightWeb"));
    CHECK_EQ(displayNameFor(true, false), QStringLiteral("MoonlightWebDev"));
    CHECK_EQ(displayNameFor(false, true), QStringLiteral("MoonlightWebDev"));
    CHECK_EQ(displayNameFor(true, true), QStringLiteral("MoonlightWebDev"));

    // The launcher's name follows the build only: a --dev run of the production
    // binary must not look for a DEV install's service or tasks.
    CHECK_EQ(productNameFor(false), QStringLiteral("MoonlightWeb"));
    CHECK_EQ(productNameFor(true), QStringLiteral("MoonlightWebDev"));

    // The version carries the suffix once, and only in DEV.
    CHECK_EQ(versionFor(QStringLiteral("0.3.0"), false), QStringLiteral("0.3.0"));
    CHECK_EQ(versionFor(QStringLiteral("0.3.0-b7c"), true), QStringLiteral("0.3.0-b7c-dev"));
    CHECK_EQ(versionFor(QStringLiteral("0.0.0-dev"), true), QStringLiteral("0.0.0-dev"));
    CHECK_EQ(versionFor(QStringLiteral("0.2.4.12.g6f79dad"), true),
             QStringLiteral("0.2.4.12.g6f79dad-dev"));

    // The DEV ports stay off the production ones and off the GameStream range.
    CHECK(kDevHttpPort != 80 && kDevHttpsPort != 443);
    CHECK(kDevSignalingPort > 48010);

    // The runner is built as the production edition, without --dev.
    char arg0[] = "run_tests";
    char* plain[] = {arg0};
    init(1, plain);
    CHECK(!isDevBuild());
    CHECK(!isDev());
    CHECK_EQ(defaultHttpsPort(), quint16(443));
    CHECK_EQ(iconAsset(QStringLiteral("favicon.ico")), QStringLiteral("assets/favicon.ico"));

    char flag[] = "--dev";
    char* withFlag[] = {arg0, flag};
    init(2, withFlag);
    CHECK(devFlag());
    CHECK(isDev());
    CHECK_EQ(dataName(), QStringLiteral("MoonlightWeb-dev"));
    CHECK_EQ(defaultHttpPort(), kDevHttpPort);
    CHECK_EQ(defaultHttpsPort(), kDevHttpsPort);
    CHECK_EQ(iconAsset(QStringLiteral("icon-512.png")), QStringLiteral("assets/dev/icon-512.png"));

    // Leave the process as the other suites expect to find it.
    init(1, plain);

    // MW_LAN_ONLY: only an explicit yes turns it on.
    CHECK(lanOnlyFrom("1"));
    CHECK(lanOnlyFrom("true"));
    CHECK(lanOnlyFrom(" YES "));
    CHECK(lanOnlyFrom("on"));
    CHECK(!lanOnlyFrom(""));
    CHECK(!lanOnlyFrom("0"));
    CHECK(!lanOnlyFrom("false"));
    CHECK(!lanOnlyFrom("off"));

    // The runner embeds nothing, so the environment alone decides.
    qputenv("MW_LAN_ONLY", "1");
    CHECK(lanOnly());
    CHECK(lanOnlyRefusal().contains(QStringLiteral("MW_LAN_ONLY")));
    qputenv("MW_LAN_ONLY", "0");
    CHECK(!lanOnly());
    qunsetenv("MW_LAN_ONLY");
    CHECK(!lanOnly());
}
