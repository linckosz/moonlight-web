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
}
