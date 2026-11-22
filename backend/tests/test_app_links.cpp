/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Links straight to one app of one host (issue #63, server/AppLinks.h): the
 * alias, the store and what it follows on its own, its routes driven without
 * the server around them, and the manifest of a home-screen shortcut to one app.
 */
#include "test_framework.h"
#include "server/AppLinks.h"
#include "server/HomeScreenShell.h"
#include "server/RequestGuard.h"
#include "server/RestRouter.h"
#include "server/routes/AppLinkRoutes.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <optional>

namespace {

const QString kHost = QStringLiteral("11111111-2222-3333-4444-555555555555");
const QString kOtherHost = QStringLiteral("99999999-8888-7777-6666-555555555555");

NvApp display(int id, const QString& name, const QString& key)
{
    NvApp app(id, name);
    app.setDevice(QJsonObject{{QStringLiteral("key"), key}});
    return app;
}

/// Aliases handed out in order, so a test knows which one it will get.
struct Aliases
{
    int next = 0;
    QString operator()() { return QStringLiteral("Alias%1").arg(next++, 5, 10, QLatin1Char('0')); }
};

QJsonObject jsonOf(const HttpResponse& resp)
{
    return QJsonDocument::fromJson(resp.body).object();
}

/// The routes as the server registers them, over a store in a temporary file
/// and hosts whose apps the test sets by hand.
struct Server
{
    QTemporaryDir tmp;
    std::shared_ptr<Aliases> aliases = std::make_shared<Aliases>();
    AppLinkStore store;
    RestRouter router;
    QHash<QString, QVector<NvApp>> hosts;
    QSet<QString> multiSeat; // hosts driven by a multi-user backend

    Server()
        : store(
              tmp.path() + QStringLiteral("/app-links.json"), [a = aliases]() { return (*a)(); },
              []() { return qint64(1'800'000'000); })
    {
        store.load();
        registerAppLinkRoutes(router, store,
                              {[this](const QString& uuid) {
                                   std::optional<QVector<NvApp>> apps;
                                   if (hosts.contains(uuid)) apps = hosts.value(uuid);
                                   return apps;
                               },
                               [this](const QString& uuid) { return multiSeat.contains(uuid); }});
    }

    HttpResponse send(const QString& method, const QString& path,
                      const QJsonObject& body = QJsonObject())
    {
        HttpRequest req;
        req.method = method;
        req.path = path;
        if (!body.isEmpty()) req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        HttpResponse reply;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }
};

} // namespace

void run_app_links_tests()
{
    SECTION("app links — an alias is ten letters and digits, never a lowercase word");
    {
        CHECK(mw::applinks::validAlias(QStringLiteral("x7Kq2AbC90")));
        CHECK(!mw::applinks::validAlias(QStringLiteral("x7Kq2AbC9")));
        CHECK(!mw::applinks::validAlias(QStringLiteral("x7Kq2AbC901")));
        CHECK(!mw::applinks::validAlias(QStringLiteral("x7Kq2AbC9-")));
        CHECK(!mw::applinks::validAlias(QStringLiteral("x7Kq2AbC9/")));
        CHECK(!mw::applinks::validAlias(QString::fromUtf8("x7Kq2AbC9é")));
        CHECK(!mw::applinks::validAlias(QString()));
        bool allValid = true;
        bool anyLowercaseWord = false;
        QSet<QString> drawn;
        for (int i = 0; i < 300; ++i) {
            const QString alias = mw::applinks::newAlias();
            allValid = allValid && mw::applinks::validAlias(alias);
            bool lower = true;
            for (const QChar c : alias)
                if (c < QLatin1Char('a') || c > QLatin1Char('z')) lower = false;
            anyLowercaseWord = anyLowercaseWord || lower;
            drawn.insert(alias);
        }
        CHECK(allValid);
        CHECK(!anyLowercaseWord);
        CHECK_EQ(drawn.size(), 300);
    }

    SECTION("app links — one link per app, kept across a restart");
    {
        QTemporaryDir tmp;
        const QString path = tmp.path() + QStringLiteral("/app-links.json");
        Aliases aliases;
        AppLinkStore store(path, [&aliases]() { return aliases(); });
        store.load();
        CHECK(store.writable());

        const auto first = store.create(kHost, NvApp(101, QStringLiteral("Desktop")));
        CHECK(first.result == AppLinkStore::CreateResult::Created);
        CHECK_EQ(first.link.alias, QStringLiteral("Alias00000"));
        const auto again = store.create(kHost, NvApp(101, QStringLiteral("Desktop")));
        CHECK(again.result == AppLinkStore::CreateResult::Existing);
        CHECK_EQ(again.link.alias, first.link.alias);
        // The same id on another host is another app.
        const auto other = store.create(kOtherHost, NvApp(101, QStringLiteral("Desktop")));
        CHECK(other.result == AppLinkStore::CreateResult::Created);
        CHECK(other.link.alias != first.link.alias);
        CHECK_EQ(store.size(), 2);

        AppLinkStore reread(path);
        reread.load();
        CHECK_EQ(reread.size(), 2);
        const auto found = reread.find(first.link.alias);
        CHECK(found.has_value());
        CHECK(found && found->hostUuid == kHost && found->appId == 101 &&
              found->name == QStringLiteral("Desktop") && found->state == AppLink::State::Present);
        CHECK(!reread.find(QStringLiteral("Unknown000")).has_value());
        CHECK(!reread.find(QStringLiteral("../../etc")).has_value());
    }

    SECTION("app links — a display of the native host is one link whatever its id");
    {
        QTemporaryDir tmp;
        AppLinkStore store(tmp.path() + QStringLiteral("/app-links.json"));
        store.load();
        const auto a = store.create(kHost, display(5, QStringLiteral("DISPLAY1"), "screen-a"));
        const auto b = store.create(kHost, display(9, QStringLiteral("DISPLAY1"), "screen-a"));
        CHECK(b.result == AppLinkStore::CreateResult::Existing);
        CHECK_EQ(b.link.alias, a.link.alias);
        CHECK_EQ(b.link.appId, 9);
        // Its id moved again: the screen's key follows it.
        store.reconcile(kHost, {display(9, "DISPLAY1", "screen-a")},
                        {display(12, "DISPLAY1", "screen-a"), NvApp(3, "Steam")});
        CHECK_EQ(store.find(a.link.alias)->appId, 12);
        CHECK(store.find(a.link.alias)->state == AppLink::State::Present);
        // The screen is gone: missing, never re-pointed at another one.
        store.reconcile(kHost, {display(12, "DISPLAY1", "screen-a")},
                        {display(13, "DISPLAY2", "screen-b")});
        CHECK(store.find(a.link.alias)->state == AppLink::State::Missing);
        CHECK_EQ(store.find(a.link.alias)->appId, 12);
    }

    SECTION("app links — what a fresh list of apps moves on its own, and what it only asks");
    {
        QTemporaryDir tmp;
        AppLinkStore store(tmp.path() + QStringLiteral("/app-links.json"));
        store.load();
        const QString desktop = store.create(kHost, NvApp(101, "Desktop")).link.alias;
        const QString steam = store.create(kHost, NvApp(202, "Steam")).link.alias;
        const QString other = store.create(kOtherHost, NvApp(101, "Desktop")).link.alias;

        // The same id under a new name: followed.
        store.reconcile(kHost, {NvApp(101, "Desktop"), NvApp(202, "Steam")},
                        {NvApp(101, "Bureau"), NvApp(202, "Steam")});
        CHECK_EQ(store.find(desktop)->name, QStringLiteral("Bureau"));
        CHECK(store.find(desktop)->state == AppLink::State::Present);
        // Another host's link is not this list's business.
        CHECK_EQ(store.find(other)->name, QStringLiteral("Desktop"));

        // The same name under a new id: followed.
        store.reconcile(kHost, {NvApp(101, "Bureau"), NvApp(202, "Steam")},
                        {NvApp(101, "Bureau"), NvApp(303, "Steam")});
        CHECK_EQ(store.find(steam)->appId, 303);
        CHECK(store.find(steam)->state == AppLink::State::Present);

        // One gone, one come, nothing else in common: asked, not followed.
        store.reconcile(kHost, {NvApp(101, "Bureau"), NvApp(303, "Steam")},
                        {NvApp(404, "Desktop 4K"), NvApp(303, "Steam")});
        auto asked = store.find(desktop);
        CHECK(asked->state == AppLink::State::ToConfirm);
        CHECK_EQ(asked->appId, 101);
        CHECK_EQ(asked->candidateId, 404);
        CHECK_EQ(asked->candidateName, QStringLiteral("Desktop 4K"));

        // A refresh that changes nothing keeps the question while its candidate
        // is there.
        store.reconcile(kHost, {NvApp(404, "Desktop 4K"), NvApp(303, "Steam")},
                        {NvApp(404, "Desktop 4K"), NvApp(303, "Steam")});
        CHECK(store.find(desktop)->state == AppLink::State::ToConfirm);

        // A yes for another app, or for one the host no longer lists, is no yes.
        const QVector<NvApp> now = {NvApp(404, "Desktop 4K"), NvApp(303, "Steam")};
        CHECK(store.confirm(desktop, 303, now) == AppLinkStore::ConfirmResult::NotPending);
        CHECK(store.confirm(desktop, 404, {NvApp(303, "Steam")}) ==
              AppLinkStore::ConfirmResult::NotPending);
        CHECK(store.confirm(QStringLiteral("Unknown000"), 404, now) ==
              AppLinkStore::ConfirmResult::Unknown);
        CHECK(store.confirm(steam, 404, now) == AppLinkStore::ConfirmResult::NotPending);
        // The yes it asked for.
        CHECK(store.confirm(desktop, 404, now) == AppLinkStore::ConfirmResult::Followed);
        CHECK_EQ(store.find(desktop)->appId, 404);
        CHECK_EQ(store.find(desktop)->name, QStringLiteral("Desktop 4K"));
        CHECK(store.find(desktop)->state == AppLink::State::Present);

        // Two gone and one come: nothing says which became which.
        store.reconcile(kHost, {NvApp(404, "Desktop 4K"), NvApp(303, "Steam")},
                        {NvApp(505, "Playnite")});
        CHECK(store.find(desktop)->state == AppLink::State::Missing);
        CHECK(store.find(steam)->state == AppLink::State::Missing);
        CHECK_EQ(store.find(steam)->missingSince, store.find(desktop)->missingSince);

        // An empty answer says nothing about the host's apps.
        store.reconcile(kHost, {NvApp(505, "Playnite")}, {});
        CHECK(store.find(desktop)->state == AppLink::State::Missing);

        // The app comes back under its name: the link finds it again.
        store.reconcile(kHost, {NvApp(505, "Playnite")}, {NvApp(606, "Steam")});
        CHECK(store.find(steam)->state == AppLink::State::Present);
        CHECK_EQ(store.find(steam)->appId, 606);
        CHECK_EQ(store.find(steam)->missingSince, qint64(0));

        // Two apps of that name: the link does not pick one.
        store.reconcile(kHost, {NvApp(606, "Steam")},
                        {NvApp(707, "Desktop 4K"), NvApp(708, "Desktop 4K")});
        CHECK(store.find(desktop)->state == AppLink::State::Missing);

        // And it all reads back the same.
        AppLinkStore reread(tmp.path() + QStringLiteral("/app-links.json"));
        reread.load();
        CHECK(reread.find(desktop)->state == AppLink::State::Missing);
        CHECK_EQ(reread.find(steam)->appId, 606);
    }

    SECTION("app links — a host removed takes its links along");
    {
        QTemporaryDir tmp;
        AppLinkStore store(tmp.path() + QStringLiteral("/app-links.json"));
        store.load();
        store.create(kHost, NvApp(1, "A"));
        store.create(kHost, NvApp(2, "B"));
        const QString kept = store.create(kOtherHost, NvApp(1, "A")).link.alias;
        CHECK_EQ(store.purgeHost(kHost), 2);
        CHECK_EQ(store.purgeHost(kHost), 0);
        CHECK_EQ(store.size(), 1);
        AppLinkStore reread(tmp.path() + QStringLiteral("/app-links.json"));
        reread.load();
        CHECK_EQ(reread.size(), 1);
        CHECK(reread.find(kept).has_value());
    }

    SECTION("app links — full means refused, never an old link dropped");
    {
        QTemporaryDir tmp;
        AppLinkStore store(tmp.path() + QStringLiteral("/app-links.json"));
        store.load();
        for (int i = 1; i <= AppLinkStore::kMaxLinks; ++i)
            store.create(kHost, NvApp(i, QStringLiteral("App %1").arg(i)));
        CHECK_EQ(store.size(), AppLinkStore::kMaxLinks);
        const auto refused = store.create(kHost, NvApp(100000, "One more"));
        CHECK(refused.result == AppLinkStore::CreateResult::Full);
        CHECK_EQ(store.size(), AppLinkStore::kMaxLinks);
        // An app that already has its link still gets it.
        CHECK(store.create(kHost, NvApp(7, "App 7")).result ==
              AppLinkStore::CreateResult::Existing);
    }

    SECTION("app links — a damaged entry costs itself; an unreadable file is never written");
    {
        QTemporaryDir tmp;
        const QString path = tmp.path() + QStringLiteral("/app-links.json");
        {
            QFile f(path);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write(R"({"version":1,"links":[
                {"alias":"Good000001","host":"h1","app_id":5,"name":"Desktop","state":"present","created":1},
                {"alias":"bad","host":"h1","app_id":6,"name":"X","state":"present"},
                {"alias":"Good000002","host":"h1","app_id":0,"name":"X","state":"present"},
                {"alias":"Good000003","host":"h1","app_id":7,"name":"X","state":"weird"},
                {"alias":"Good000001","host":"h2","app_id":8,"name":"Twice","state":"present"},
                {"alias":"Good000004","host":"h1","app_id":9,"name":"Y","state":"confirm"},
                42
            ]})");
        }
        AppLinkStore store(path);
        store.load();
        CHECK(store.writable());
        CHECK_EQ(store.size(), 2);
        CHECK_EQ(store.find(QStringLiteral("Good000001"))->name, QStringLiteral("Desktop"));
        // A question with no candidate is only a missing app.
        CHECK(store.find(QStringLiteral("Good000004"))->state == AppLink::State::Missing);

        const QByteArray garbage = "{ this is not json";
        {
            QFile f(path);
            CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(garbage);
        }
        AppLinkStore broken(path);
        broken.load();
        CHECK(!broken.writable());
        CHECK(broken.create(kHost, NvApp(1, "A")).result == AppLinkStore::CreateResult::Unwritable);
        CHECK_EQ(broken.purgeHost(QStringLiteral("h1")), 0);
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK_EQ(f.readAll(), garbage);
    }

    SECTION("app links — the routes: made, followed, confirmed");
    {
        Server s;
        s.hosts.insert(kHost, {NvApp(101, "Desktop"), NvApp(202, "Steam")});

        // What a body must be.
        CHECK_EQ(s.send("POST", "/api/app-links").statusCode, 400);
        CHECK_EQ(s.send("POST", "/api/app-links", {{"host_uuid", kHost}}).statusCode, 400);
        CHECK_EQ(
            s.send("POST", "/api/app-links", {{"host_uuid", kHost}, {"app_id", "101"}}).statusCode,
            400);
        CHECK_EQ(
            s.send("POST", "/api/app-links", {{"host_uuid", kHost}, {"app_id", 1.5}}).statusCode,
            400);
        // A host or an app this server does not list.
        CHECK_EQ(s.send("POST", "/api/app-links", {{"host_uuid", kOtherHost}, {"app_id", 101}})
                     .statusCode,
                 404);
        CHECK_EQ(
            s.send("POST", "/api/app-links", {{"host_uuid", kHost}, {"app_id", 999}}).statusCode,
            404);

        const HttpResponse made =
            s.send("POST", "/api/app-links", {{"host_uuid", kHost}, {"app_id", 101}});
        CHECK_EQ(made.statusCode, 200);
        const QString alias = jsonOf(made).value("alias").toString();
        CHECK(mw::applinks::validAlias(alias));
        CHECK_EQ(jsonOf(s.send("POST", "/api/app-links", {{"host_uuid", kHost}, {"app_id", 101}}))
                     .value("alias")
                     .toString(),
                 alias);

        const HttpResponse got = s.send("GET", "/api/app-links/" + alias);
        CHECK_EQ(got.statusCode, 200);
        const QJsonObject where = jsonOf(got);
        CHECK_EQ(where.value("host_uuid").toString(), kHost);
        CHECK_EQ(where.value("app_id").toInt(), 101);
        CHECK_EQ(where.value("name").toString(), QStringLiteral("Desktop"));
        CHECK_EQ(where.value("state").toString(), QStringLiteral("present"));
        CHECK(!where.contains("alias"));

        // Unknown, ill-shaped, and a host that is gone: the same 404.
        const HttpResponse unknown = s.send("GET", "/api/app-links/Unknown000");
        const HttpResponse shape = s.send("GET", "/api/app-links/no");
        CHECK_EQ(unknown.statusCode, 404);
        CHECK_EQ(shape.statusCode, 404);
        CHECK_EQ(unknown.body, shape.body);

        // Renamed with a new id and nothing in common: the page is asked.
        s.store.reconcile(kHost, s.hosts.value(kHost), {NvApp(404, "Bureau"), NvApp(202, "Steam")});
        s.hosts.insert(kHost, {NvApp(404, "Bureau"), NvApp(202, "Steam")});
        const QJsonObject pending = jsonOf(s.send("GET", "/api/app-links/" + alias));
        CHECK_EQ(pending.value("state").toString(), QStringLiteral("confirm"));
        CHECK_EQ(pending.value("candidate").toObject().value("app_id").toInt(), 404);
        CHECK_EQ(pending.value("candidate").toObject().value("name").toString(),
                 QStringLiteral("Bureau"));

        CHECK_EQ(s.send("POST", "/api/app-links/" + alias + "/confirm").statusCode, 400);
        CHECK_EQ(
            s.send("POST", "/api/app-links/" + alias + "/confirm", {{"app_id", 202}}).statusCode,
            409);
        CHECK_EQ(s.send("POST", "/api/app-links/Unknown000/confirm", {{"app_id", 404}}).statusCode,
                 404);
        const HttpResponse yes =
            s.send("POST", "/api/app-links/" + alias + "/confirm", {{"app_id", 404}});
        CHECK_EQ(yes.statusCode, 200);
        CHECK_EQ(jsonOf(yes).value("app_id").toInt(), 404);
        CHECK_EQ(jsonOf(yes).value("state").toString(), QStringLiteral("present"));
        // Asked twice: nothing left to confirm.
        CHECK_EQ(
            s.send("POST", "/api/app-links/" + alias + "/confirm", {{"app_id", 404}}).statusCode,
            409);

        s.hosts.remove(kHost);
        const HttpResponse gone = s.send("GET", "/api/app-links/" + alias);
        CHECK_EQ(gone.statusCode, 404);
        CHECK_EQ(gone.body, unknown.body);
    }

    SECTION("app links — none on a multi-seat host, where the apps are a seat's");
    {
        Server s;
        s.hosts.insert(kHost, {NvApp(101, "Desktop")});
        s.multiSeat.insert(kHost);
        const HttpResponse refused =
            s.send("POST", "/api/app-links", {{"host_uuid", kHost}, {"app_id", 101}});
        CHECK_EQ(refused.statusCode, 409);
        CHECK_EQ(jsonOf(refused).value("error").toString(), QStringLiteral("not_on_multi_seat"));
        CHECK_EQ(s.store.size(), 0);
        CHECK(!QFile::exists(s.tmp.path() + QStringLiteral("/app-links.json")));

        // Routes registered without saying which hosts are multi-seat refuse
        // every one: unknown counts as yes.
        QTemporaryDir tmp;
        AppLinkStore store(tmp.path() + QStringLiteral("/app-links.json"));
        store.load();
        RestRouter router;
        registerAppLinkRoutes(router, store, {[](const QString&) {
                                  return std::optional<QVector<NvApp>>(
                                      QVector<NvApp>{NvApp(101, "Desktop")});
                              }});
        HttpRequest req;
        req.method = QStringLiteral("POST");
        req.path = QStringLiteral("/api/app-links");
        req.body = QJsonDocument(QJsonObject{{"host_uuid", kHost}, {"app_id", 101}}).toJson();
        HttpResponse reply;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        CHECK_EQ(reply.statusCode, 409);
        CHECK_EQ(store.size(), 0);
    }

    SECTION("app links — no door of their own: not the local channel's, locked in a booth");
    {
        CHECK(!RequestGuard::localChannelRoute("POST", "/api/app-links"));
        CHECK(!RequestGuard::localChannelRoute("GET", "/api/app-links/x7Kq2AbC90"));
        CHECK(RequestGuard::lockedByNoAdmin("POST", "/api/app-links"));
        CHECK(RequestGuard::lockedByNoAdmin("POST", "/api/app-links/x7Kq2AbC90/confirm"));
        CHECK(!RequestGuard::lockedByNoAdmin("GET", "/api/app-links/x7Kq2AbC90"));
    }

    SECTION("app links — the manifest of a shortcut to one app");
    {
        namespace hs = mw::homescreen;
        const QByteArray json = R"({
            "name": "MoonlightWeb", "start_url": "/", "scope": "/",
            "icons": [ { "src": "assets/icon-192.png" }, { "src": "assets/icon-512.png" } ]
        })";
        const QString id = QStringLiteral("0123456789abcdefghjkmnpqrs");

        // Off the tunnel: this origin's "/", the link in the fragment, an id of
        // its own, the icons inlined, and no name at all — the phone names the
        // icon after the page that made it.
        const QJsonObject lan =
            QJsonDocument::fromJson(hs::manifest(json, QString(), QString(), QString(), "P192",
                                                 "P512", QStringLiteral("x7Kq2AbC90")))
                .object();
        CHECK_EQ(lan["start_url"].toString(), QStringLiteral("/#go=x7Kq2AbC90"));
        CHECK_EQ(lan["id"].toString(), QStringLiteral("/?go=x7Kq2AbC90"));
        CHECK(!lan.contains("name"));
        CHECK(!lan.contains("short_name"));
        CHECK(lan["icons"].toArray().at(0).toObject()["src"].toString().startsWith("data:"));

        // Through the tunnel: the machine's address, the link, then the key —
        // and no name there either, not even the machine's.
        const QJsonObject tunnel =
            QJsonDocument::fromJson(hs::manifest(json, "MoonlightWeb [DualRTX]", id, "K_1", "P192",
                                                 "P512", QStringLiteral("x7Kq2AbC90")))
                .object();
        CHECK_EQ(tunnel["start_url"].toString(), "/" + id + "#go=x7Kq2AbC90&k=K_1");
        CHECK_EQ(tunnel["id"].toString(), "/" + id + "?go=x7Kq2AbC90");
        CHECK(!tunnel.contains("name"));
        CHECK(!tunnel.contains("short_name"));

        // Without a link, exactly as before.
        const QJsonObject plain =
            QJsonDocument::fromJson(hs::manifest(json, QString(), id, "K_1", "P192", "P512"))
                .object();
        CHECK_EQ(plain["start_url"].toString(), "/" + id + "#k=K_1");
        CHECK_EQ(plain["id"].toString(), "/" + id);
        const QJsonObject untouched =
            QJsonDocument::fromJson(hs::manifest(json, QString(), QString(), QString(), "", ""))
                .object();
        CHECK_EQ(untouched["start_url"].toString(), QStringLiteral("/"));
        CHECK(!untouched.contains("id"));

        // Anything that is not an alias is never written into the address.
        const QJsonObject bad =
            QJsonDocument::fromJson(hs::manifest(json, QString(), QString(), QString(), "", "",
                                                 QStringLiteral("x7Kq2#k=AA")))
                .object();
        CHECK_EQ(bad["start_url"].toString(), QStringLiteral("/"));
    }

    SECTION("app links — the icon's shell and manifest, the direct way in only");
    {
        namespace hs = mw::homescreen;
        using Part = hs::AppIconPart;
        const hs::IconReach direct = hs::IconReach::DirectToHost;
        const hs::IconReach elsewhere = hs::IconReach::Elsewhere;
        const QString alias = QStringLiteral("x7Kq2AbC90");

        // The shell for "/" or "/index.html" with ?icon=, the manifest for
        // /manifest.webmanifest with ?go=; nothing else, and only the direct way
        // in — never by the tunnel, nor any other way.
        CHECK(hs::appIconPart("/", alias, QString(), direct) == Part::Shell);
        CHECK(hs::appIconPart("/index.html", alias, QString(), direct) == Part::Shell);
        CHECK(hs::appIconPart("/manifest.webmanifest", QString(), alias, direct) == Part::Manifest);
        CHECK(hs::appIconPart("/", alias, QString(), elsewhere) == Part::None);
        CHECK(hs::appIconPart("/index.html", alias, QString(), elsewhere) == Part::None);
        CHECK(hs::appIconPart("/manifest.webmanifest", QString(), alias, elsewhere) == Part::None);
        CHECK(hs::appIconPart("/admin", alias, QString(), direct) == Part::None);
        CHECK(hs::appIconPart("/", QString(), alias, direct) == Part::None);
        CHECK(hs::appIconPart("/manifest.webmanifest", alias, QString(), direct) == Part::None);
        CHECK(hs::appIconPart("/js/app.js", alias, alias, direct) == Part::None);

        const QByteArray html =
            "<head>\n"
            "<meta name=\"apple-mobile-web-app-title\" content=\"MoonlightWeb\" />\n"
            "<link rel=\"manifest\" href=\"/manifest.webmanifest\" />\n"
            "</head>";
        const QByteArray json = R"({
            "name": "MoonlightWeb", "short_name": "MoonlightWeb", "start_url": "/", "scope": "/",
            "icons": [ { "src": "assets/icon-192.png" }, { "src": "assets/icon-512.png" } ]
        })";

        // The shell names the manifest that opens the link, and nothing else moves.
        const QByteArray shell = hs::appIconShell(html, alias);
        CHECK(shell.contains(
            "<link rel=\"manifest\" href=\"/manifest.webmanifest?go=x7Kq2AbC90\" />"));
        CHECK_EQ(QByteArray(shell).replace("?go=x7Kq2AbC90", ""), html);

        // Anything but an alias's shape: the files exactly as they are, byte for
        // byte — a quote, an angle bracket, an escape, a path, too short, too long.
        const QStringList notAliases = {QStringLiteral("x7Kq2\"bC90"),
                                        QStringLiteral("x7Kq2<bC90"),
                                        QStringLiteral("%22Kq2AbC90"),
                                        QStringLiteral("../Kq2AbC9"),
                                        QStringLiteral("x7Kq2AbC9"),
                                        QStringLiteral("x7Kq2AbC90x"),
                                        QString(1000, QLatin1Char('a')),
                                        QStringLiteral("x7Kq2 bC90"),
                                        QStringLiteral("x7Kq2&bC90"),
                                        QStringLiteral("x7Kq2#bC90"),
                                        QStringLiteral("x7Kq2?bC90"),
                                        QStringLiteral("x7Kq2/bC90"),
                                        QString::fromUtf8("x7Kq2\xC3\xA9"
                                                          "C90a"),
                                        QString()};
        for (const QString& notAlias : notAliases) {
            CHECK(hs::appIconPart("/", notAlias, QString(), direct) == Part::None);
            CHECK(hs::appIconPart("/manifest.webmanifest", QString(), notAlias, direct) ==
                  Part::None);
            CHECK_EQ(hs::appIconShell(html, notAlias), html);
            CHECK_EQ(hs::manifest(json, QString(), QString(), QString(), "", "", notAlias),
                     hs::manifest(json, QString(), QString(), QString(), "", ""));
        }

        // The manifest a request without a session reads: the alias and nothing
        // else — no key, no machine, no identifier, no name; this origin's "/".
        const QByteArray bytes =
            hs::manifest(json, QString(), QString(), QString(), "P192", "P512", alias);
        const QJsonObject m = QJsonDocument::fromJson(bytes).object();
        CHECK_EQ(m["start_url"].toString(), QStringLiteral("/#go=x7Kq2AbC90"));
        CHECK_EQ(m["id"].toString(), QStringLiteral("/?go=x7Kq2AbC90"));
        CHECK_EQ(m["scope"].toString(), QStringLiteral("/"));
        CHECK(!m.contains("name"));
        CHECK(!m.contains("short_name"));
        CHECK(!bytes.contains("k="));
        CHECK(!bytes.contains("MoonlightWeb"));
        CHECK(!bytes.contains("DualRTX"));
    }

    // I63NAME (security review, C1-C10): the icon's name is the one the page that
    // made it wrote into the request, given back and never looked up. It is a
    // label: the alias alone says what the icon opens.
    SECTION("app links — the icon's name, given back as the request wrote it");
    {
        namespace hs = mw::homescreen;
        const QString alias = QStringLiteral("x7Kq2AbC90");
        const QString name = QString::fromUtf8("DualRTX \xC2\xB7 Steam \xC2\xB7 MoonlightWeb");
        const QByteArray html =
            "<head>\n"
            "<title>MoonlightWeb</title>\n"
            "<meta name=\"apple-mobile-web-app-title\" content=\"MoonlightWeb\" />\n"
            "<link rel=\"manifest\" href=\"/manifest.webmanifest\" />\n"
            "</head>";
        const QByteArray json = R"({
            "name": "MoonlightWeb", "short_name": "MoonlightWeb", "start_url": "/", "scope": "/",
            "icons": [ { "src": "assets/icon-192.png" }, { "src": "assets/icon-512.png" } ]
        })";

        // In the manifest, as JSON: the name whole, the short name cut to 12
        // code points; where it opens and its id the alias's alone (C6).
        const QJsonObject named =
            QJsonDocument::fromJson(
                hs::manifest(json, QString(), QString(), QString(), "", "", alias, name))
                .object();
        const QJsonObject bare = QJsonDocument::fromJson(hs::manifest(json, QString(), QString(),
                                                                      QString(), "", "", alias))
                                     .object();
        CHECK_EQ(named["name"].toString(), name);
        CHECK_EQ(named["short_name"].toString(), QString::fromUtf8("DualRTX \xC2\xB7 St"));
        CHECK_EQ(named["start_url"].toString(), bare["start_url"].toString());
        CHECK_EQ(named["id"].toString(), bare["id"].toString());
        CHECK_EQ(named["scope"].toString(), bare["scope"].toString());

        // In the shell, only in the manifest's address, encoded down to
        // A-Z a-z 0-9 - . _ ~ (C7): nothing else of the page moves, and the
        // title and the apple title keep the edition's.
        const QByteArray shell = hs::appIconShell(html, alias, name);
        CHECK(shell.contains("<link rel=\"manifest\" href=\"/manifest.webmanifest?go=x7Kq2AbC90"
                             "&amp;n=DualRTX%20%C2%B7%20Steam%20%C2%B7%20MoonlightWeb\" />"));
        CHECK(shell.contains("<title>MoonlightWeb</title>"));
        CHECK(shell.contains("content=\"MoonlightWeb\""));
        CHECK(!shell.contains("Steam "));

        // A hostile name (C10): refused whole, never cleaned — the manifest and
        // the shell are then the ones with no name, byte for byte. A quote, an
        // angle bracket, an ampersand that survive are escaped or encoded.
        const QStringList refused = {
            QString(65, QLatin1Char('a')),             // 65 code points
            QStringLiteral("   "),                     // blank
            QStringLiteral("a") + QChar(0x0000) + "b", // NUL (%00 decoded)
            QStringLiteral("a\tb"),                    // a control
            QStringLiteral("a") + QChar(0x007F) + "b", // DEL
            QStringLiteral("a") + QChar(0x0085) + "b", // a C1 control
            QStringLiteral("a") + QChar(0x202E) + "b", // RIGHT-TO-LEFT OVERRIDE
            QStringLiteral("a") + QChar(0x2067) + "b", // RIGHT-TO-LEFT ISOLATE
            QStringLiteral("a") + QChar(0x200B) + "b", // ZERO WIDTH SPACE
            QStringLiteral("a") + QChar(0x200F) + "b", // RIGHT-TO-LEFT MARK
            QStringLiteral("a") + QChar(0x2028) + "b", // LINE SEPARATOR
            QStringLiteral("a") + QChar(0x2029) + "b", // PARAGRAPH SEPARATOR
            QStringLiteral("a") + QChar(0xFEFF) + "b", // BYTE ORDER MARK
            QString::fromUtf8("a\xFF\xFE"
                              "b"),                    // invalid UTF-8 (U+FFFD)
            QStringLiteral("a") + QChar(0xD800) + "b", // a lone surrogate
        };
        for (const QString& hostile : refused) {
            CHECK(hs::appIconName(hostile).isEmpty());
            CHECK(hs::appIconShortName(hostile).isEmpty());
            CHECK_EQ(hs::manifest(json, QString(), QString(), QString(), "", "", alias, hostile),
                     hs::manifest(json, QString(), QString(), QString(), "", "", alias));
            CHECK_EQ(hs::appIconShell(html, alias, hostile), hs::appIconShell(html, alias));
        }
        CHECK_EQ(hs::appIconName(QString(64, QLatin1Char('a'))), QString(64, QLatin1Char('a')));
        // Counted in code points: 64 emoji, each two UTF-16 units, hold.
        QString emoji;
        for (int i = 0; i < 64; ++i)
            emoji += QString::fromUtf8("\xF0\x9F\x8E\xAE");
        CHECK_EQ(hs::appIconName(emoji), emoji);
        CHECK_EQ(hs::appIconName(emoji + "a"), QString());
        // Cut between two code points, never inside a pair.
        CHECK_EQ(hs::appIconShortName(emoji), emoji.left(24));

        const QString script = QStringLiteral("<script>alert(\"x\")</script> & 'q'");
        const QByteArray scripted = hs::appIconShell(html, alias, script);
        CHECK(!scripted.contains("<script"));
        CHECK(!scripted.contains("\"x\""));
        CHECK(!scripted.contains("'q'"));
        CHECK(scripted.contains(
            "n=%3Cscript%3Ealert%28%22x%22%29%3C%2Fscript%3E%20%26%20%27q%27\" />"));
        const QByteArray scriptedJson =
            hs::manifest(json, QString(), QString(), QString(), "", "", alias, script);
        CHECK_EQ(QJsonDocument::fromJson(scriptedJson).object()["name"].toString(), script);
        CHECK_EQ(QJsonDocument::fromJson(scriptedJson).object()["start_url"].toString(),
                 QStringLiteral("/#go=x7Kq2AbC90"));

        // The short name never leaves a letter without the marks that follow
        // it: "e" and its combining acute at the cut go together.
        const QString marked =
            QStringLiteral("abcdefghijk") + QStringLiteral("e") + QChar(0x0301) + "xyz";
        CHECK_EQ(hs::appIconShortName(marked), QStringLiteral("abcdefghijk"));
        CHECK_EQ(hs::appIconShortName(QStringLiteral("abcdefghijkl") + QChar(0x0301)),
                 QStringLiteral("abcdefghijk"));

        // As the request carries it (C9): what HttpParser left encoded is
        // decoded here, %00 becomes a NUL that appIconName then refuses, and
        // past 512 bytes encoded the name is not read at all.
        CHECK_EQ(hs::appIconNameAsked(QStringLiteral("Steam %26 co")),
                 QStringLiteral("Steam & co"));
        CHECK_EQ(hs::appIconNameAsked(QStringLiteral("100%25")), QStringLiteral("100%"));
        CHECK(hs::appIconName(hs::appIconNameAsked(QStringLiteral("a%00b"))).isEmpty());
        CHECK(hs::appIconName(hs::appIconNameAsked(QStringLiteral("a%E2%80%AEb"))).isEmpty());
        CHECK(hs::appIconName(hs::appIconNameAsked(QStringLiteral("a%FFb"))).isEmpty());
        CHECK_EQ(hs::appIconNameAsked(QString(512, QLatin1Char('a'))),
                 QString(512, QLatin1Char('a')));
        CHECK(hs::appIconNameAsked(QString(513, QLatin1Char('a'))).isEmpty());
        CHECK(hs::appIconNameAsked(QString(171, QChar(0x00E9))).isEmpty()); // 171 x %C3%A9 > 512

        // A name with no link: not written anywhere, the machine's manifest as it was.
        CHECK_EQ(hs::manifest(json, QString(), QString(), QString(), "", "", QString(), name),
                 hs::manifest(json, QString(), QString(), QString(), "", ""));
        CHECK_EQ(hs::appIconShell(html, QStringLiteral("x7Kq2\"bC90"), name), html);
    }
}
