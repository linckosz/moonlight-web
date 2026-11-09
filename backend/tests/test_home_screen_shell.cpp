/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A home-screen shortcut made through the introduction server: its name, the
 * address it opens and its icon all come from the shell and the manifest the
 * host rewrites for the tunnel (server/HomeScreenShell.h).
 */
#include "test_framework.h"
#include "server/HomeScreenShell.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace hs = mw::homescreen;

void run_home_screen_shell_tests()
{
    SECTION("home screen — the shortcut names the machine it opens");
    CHECK_EQ(hs::title("MoonlightWebDev", "DualRTX"), QString("MoonlightWebDev [DualRTX]"));
    CHECK_EQ(hs::title("MoonlightWeb", "  "), QString("MoonlightWeb"));

    SECTION("home screen — the machine's name for a signed-in session only");
    CHECK_EQ(hs::shownMachineName("DualRTX", true), QString("DualRTX"));
    CHECK(hs::shownMachineName("DualRTX", false).isEmpty());
    // Without one, the shell and the manifest say the edition's name alone.
    CHECK_EQ(hs::title("MoonlightWeb", hs::shownMachineName("DualRTX", false)),
             QString("MoonlightWeb"));

    const QByteArray manifestJson = R"({
        "name": "MoonlightWeb",
        "short_name": "MoonlightWeb",
        "start_url": "/",
        "scope": "/",
        "launch_handler": { "client_mode": ["navigate-existing", "auto"] },
        "icons": [
            { "src": "assets/icon-192.png", "sizes": "192x192" },
            { "src": "assets/icon-512.png", "sizes": "512x512" }
        ]
    })";
    const QString id = "0123456789abcdefghjkmnpqrs";

    SECTION("home screen — the manifest opens this machine, under its name");
    {
        const QJsonObject m =
            QJsonDocument::fromJson(hs::manifest(manifestJson, "MoonlightWeb [DualRTX]", id,
                                                 "hs-Ab_9", QByteArray("P192"), QByteArray("P512")))
                .object();
        CHECK_EQ(m["name"].toString(), QString("MoonlightWeb [DualRTX]"));
        CHECK_EQ(m["short_name"].toString(), QString("MoonlightWeb [DualRTX]"));
        // The key rides in the fragment; the application id stays bare, so a
        // fresh key is the same application rather than a second one.
        CHECK_EQ(m["start_url"].toString(), "/" + id + "#k=hs-Ab_9");
        CHECK_EQ(m["id"].toString(), "/" + id);
        // The scope stays the origin: the entry page hands over to "/".
        CHECK_EQ(m["scope"].toString(), QString("/"));
        // An installed application opened from a link (a TV's QR code) takes the
        // window it already has, where the browser allows it, not a second one.
        CHECK_EQ(m["launch_handler"].toObject()["client_mode"].toArray().at(0).toString(),
                 QString("navigate-existing"));
        const QJsonArray icons = m["icons"].toArray();
        CHECK_EQ(icons.at(0).toObject()["src"].toString(),
                 "data:image/png;base64," + QString::fromLatin1(QByteArray("P192").toBase64()));
        CHECK_EQ(icons.at(1).toObject()["src"].toString(),
                 "data:image/png;base64," + QString::fromLatin1(QByteArray("P512").toBase64()));
    }

    SECTION("home screen — no identifier, no invented address; no bytes, no icon change");
    {
        const QJsonObject m =
            QJsonDocument::fromJson(hs::manifest(manifestJson, "MoonlightWeb", "not-an-id", "hs-x",
                                                 QByteArray(), QByteArray()))
                .object();
        CHECK_EQ(m["start_url"].toString(), QString("/"));
        CHECK(!m.contains("id"));
        CHECK_EQ(m["icons"].toArray().at(0).toObject()["src"].toString(),
                 QString("assets/icon-192.png"));
        CHECK_EQ(hs::manifest("not json", "X", id, {}, {}, {}), QByteArray("not json"));
        // No key: the bare address, and the shortcut asks for the PIN.
        CHECK_EQ(QJsonDocument::fromJson(hs::manifest(manifestJson, "X", id, {}, {}, {}))
                     .object()["start_url"]
                     .toString(),
                 "/" + id);
    }

    SECTION("home screen — the shell carries the name and the icon itself");
    {
        const QByteArray html =
            "<head>\n"
            "<meta name=\"apple-mobile-web-app-title\" content=\"MoonlightWeb\" />\n"
            "<link rel=\"manifest\" href=\"/manifest.webmanifest\" />\n"
            "<link rel=\"apple-touch-icon\" sizes=\"180x180\" href=\"/assets/icon-180.png\" />\n"
            "</head>";
        const QString out = QString::fromUtf8(hs::shell(html, "A&B [\\1 \"x\"]", "P180"));
        CHECK(out.contains("content=\"A&amp;B [\\1 &quot;x&quot;]\""));
        CHECK(out.contains("href=\"data:image/png;base64," +
                           QString::fromLatin1(QByteArray("P180").toBase64()) + "\""));
        // The manifest link stays on the file: the service worker's cache
        // answers it while the page is paused behind the share sheet.
        CHECK(out.contains("rel=\"manifest\" href=\"/manifest.webmanifest\""));
        CHECK(!out.contains("/assets/icon-180.png"));
    }
}
