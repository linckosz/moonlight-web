/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * StaticFileHandler is a security boundary: it maps untrusted request paths to
 * files under a single root and must never serve anything outside it. These
 * tests exercise the traversal guards, the SPA-friendly serving, and the
 * MIME / Cache-Control behaviour.
 */
#include "test_framework.h"
#include "server/StaticFileHandler.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

static void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        f.close();
    }
}

void run_static_files_tests()
{
    SECTION("StaticFileHandler");

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    // Lay out a small web root and a secret file OUTSIDE it (sibling dir), so a
    // successful traversal would be observable.
    const QString root = tmp.path() + "/web";
    QDir().mkpath(root);
    QDir().mkpath(root + "/sub");
    writeFile(root + "/index.html", "<!doctype html>root");
    writeFile(root + "/app.js", "console.log(1)");
    writeFile(root + "/style.css", "body{}");
    writeFile(root + "/sub/page.html", "nested");
    writeFile(tmp.path() + "/secret.txt", "TOP SECRET");

    StaticFileHandler h(root);

    // "/" and "" resolve to index.html.
    HttpResponse rootResp = h.serveFile("/");
    CHECK_EQ(rootResp.statusCode, 200);
    CHECK(rootResp.body.contains("root"));
    CHECK_EQ(h.serveFile("").statusCode, 200);

    // Normal files are served with the right content and MIME type.
    HttpResponse js = h.serveFile("/app.js");
    CHECK_EQ(js.statusCode, 200);
    CHECK(js.contentType.startsWith("application/javascript"));
    CHECK(h.serveFile("/style.css").contentType.startsWith("text/css"));

    // Nested legit path works.
    CHECK_EQ(h.serveFile("/sub/page.html").statusCode, 200);

    // Traversal attempts must be rejected (403), never leak the sibling secret.
    for (const QString& evil : {QStringLiteral("/../secret.txt"),
                                QStringLiteral("/sub/../../secret.txt"),
                                QStringLiteral("/..%2fsecret.txt"), QStringLiteral("/../")}) {
        HttpResponse r = h.serveFile(evil);
        CHECK(r.statusCode == 403 || r.statusCode == 404);
        CHECK(!r.body.contains("TOP SECRET"));
    }

    // Missing files → 404 (not 500, not a leak).
    CHECK_EQ(h.serveFile("/does-not-exist.js").statusCode, 404);

    // Unknown extension → generic octet-stream.
    writeFile(root + "/blob.bin", "x");
    CHECK(h.serveFile("/blob.bin").contentType.startsWith("application/octet-stream"));

    // Text assets are marked no-cache (iOS WebKit staleness fix); html/js/css.
    CHECK(h.serveFile("/index.html").headers.value("Cache-Control").contains("no-cache"));
    CHECK(h.serveFile("/app.js").headers.value("Cache-Control").contains("no-cache"));

    // ── Conditional requests: ETag / Last-Modified / 304 ──────────────────────
    // A 200 carries validators, and replaying the ETag yields a bodyless 304 so
    // an up-to-date browser skips re-downloading the asset after an app update.
    HttpResponse full = h.serveFile("/app.js");
    const QString etag = full.headers.value("ETag");
    CHECK(!etag.isEmpty());
    CHECK(!full.headers.value("Last-Modified").isEmpty());

    HttpResponse notMod = h.serveFile("/app.js", etag);
    CHECK_EQ(notMod.statusCode, 304);
    CHECK(notMod.body.isEmpty());
    CHECK_EQ(notMod.headers.value("ETag"), etag);

    // A stale/absent validator still gets the full body (200).
    CHECK_EQ(h.serveFile("/app.js", "\"stale\"").statusCode, 200);

    // ── Version stamping: __MW_VERSION__ in HTML → running version ────────────
    // The token is resolved only in HTML (so `css/x.css?v=__MW_VERSION__`
    // becomes a per-release cache-bust); non-HTML bytes are served verbatim.
    writeFile(root + "/stamped.html", "<link href=\"a.css?v=__MW_VERSION__\">");
    writeFile(root + "/stamped.js", "const v='__MW_VERSION__';");
    StaticFileHandler hv(root, "9.9.9");
    HttpResponse stampedHtml = hv.serveFile("/stamped.html");
    CHECK(stampedHtml.body.contains("a.css?v=9.9.9"));
    CHECK(!stampedHtml.body.contains("__MW_VERSION__"));
    // The version is part of the ETag, so a different build busts the cache.
    CHECK(stampedHtml.headers.value("ETag").contains("9.9.9"));
    // JS is not rewritten — only HTML carries the stamp.
    CHECK(hv.serveFile("/stamped.js").body.contains("__MW_VERSION__"));
}
