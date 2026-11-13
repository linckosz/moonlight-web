/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The list of files that make up the application.
 *
 * It exists for browsers reached through the rendezvous, which have no HTTP
 * route to the host and so cannot discover the application by following links.
 * They pull everything this list names, in one pass, over a data channel, before
 * the page can start.
 *
 * That is what makes the exclusions load-bearing rather than tidy. A developer
 * running from the source tree has a coverage report and a node_modules beside
 * the application; naming those would not be untidiness, it would be a minute of
 * waiting and megabytes of somebody's connection. And a dot file is exactly what
 * a scanner probes for, so publishing a list of them would be worse than useless.
 */
#include "test_framework.h"

#include "../src/server/StaticFileHandler.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(content);
}

} // namespace

void run_app_manifest_tests()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path() + QLatin1Char('/');

    // What a real frontend directory looks like when it is the SOURCE tree —
    // the case a developer runs, and the one that goes wrong quietly.
    writeFile(root + "index.html", "<!doctype html>");
    writeFile(root + "js/app.js", "export {}");
    writeFile(root + "js/util/deep/nested.js", "export {}");
    writeFile(root + "css/base.css", "body{}");
    writeFile(root + "assets/logo.png", "\x89PNG");

    writeFile(root + ".prettierrc.json", "{}");
    writeFile(root + ".claude/notes.md", "internal");
    writeFile(root + "node_modules/left-pad/index.js", "module.exports=0");
    writeFile(root + "coverage/index.html", "<html>");
    writeFile(root + "test/app.test.js", "test()");
    writeFile(root + "scripts/build.sh", "#!/bin/sh");
    writeFile(root + "build/out.js", "compiled");
    writeFile(root + "js/app.js.map", "{\"version\":3}");
    // Tool configuration at the root, which install() leaves out of a package.
    writeFile(root + "package.json", "{}");
    writeFile(root + "package-lock.json", "{}");
    writeFile(root + "eslint.config.js", "export default []");
    writeFile(root + "tsconfig.json", "{}");
    writeFile(root + "vitest.config.js", "export default {}");
    writeFile(root + "prompt-frontend.md", "notes");

    const StaticFileHandler handler(root, QStringLiteral("1.2.3"));
    const QStringList files = handler.listFiles();

    SECTION("app manifest — every file the application is actually made of");

    CHECK(files.contains(QStringLiteral("/index.html")));
    CHECK(files.contains(QStringLiteral("/js/app.js")));
    CHECK(files.contains(QStringLiteral("/css/base.css")));
    CHECK(files.contains(QStringLiteral("/assets/logo.png")));

    // Depth is the point of walking the tree rather than reading index.html: a
    // module imported by a module is invisible to any parse of the entry page,
    // and it is most of the application.
    CHECK(files.contains(QStringLiteral("/js/util/deep/nested.js")));

    SECTION("app manifest — nothing that exists only for the developer");

    CHECK(!files.contains(QStringLiteral("/.prettierrc.json")));
    CHECK(!files.contains(QStringLiteral("/.claude/notes.md")));
    CHECK(!files.contains(QStringLiteral("/node_modules/left-pad/index.js")));
    CHECK(!files.contains(QStringLiteral("/coverage/index.html")));
    CHECK(!files.contains(QStringLiteral("/test/app.test.js")));
    CHECK(!files.contains(QStringLiteral("/scripts/build.sh")));
    CHECK(!files.contains(QStringLiteral("/build/out.js")));
    CHECK(!files.contains(QStringLiteral("/package.json")));
    CHECK(!files.contains(QStringLiteral("/package-lock.json")));
    CHECK(!files.contains(QStringLiteral("/eslint.config.js")));
    CHECK(!files.contains(QStringLiteral("/tsconfig.json")));
    CHECK(!files.contains(QStringLiteral("/vitest.config.js")));
    CHECK(!files.contains(QStringLiteral("/prompt-frontend.md")));

    // Served to whoever opens the debugger, never in the set every visitor
    // downloads before the page appears.
    CHECK(!files.contains(QStringLiteral("/js/app.js.map")));

    SECTION("app manifest — what is not listed is not served either");

    // A run from the source tree serves what a package ships, no more: asking by
    // name gets the same answer as a file that is not there.
    for (const char* path :
         {"/package-lock.json", "/package.json", "/eslint.config.js", "/tsconfig.json",
          "/vitest.config.js", "/prompt-frontend.md", "/.prettierrc.json", "/.claude/notes.md",
          "/test/app.test.js", "/node_modules/left-pad/index.js", "/coverage/index.html",
          "/scripts/build.sh", "/build/out.js", "/PACKAGE-LOCK.JSON"})
        CHECK_EQ(handler.serveFile(QString::fromLatin1(path)).statusCode, 404);
    // Same file, other spellings of its path.
    CHECK_EQ(handler.serveFile(QStringLiteral("//package-lock.json")).statusCode, 404);
    CHECK_EQ(handler.serveFile(QStringLiteral("/./package-lock.json")).statusCode, 404);
    CHECK(handler.serveFile(QStringLiteral("/js/../package-lock.json")).statusCode >= 400);
#ifdef Q_OS_WIN
    CHECK_EQ(handler.serveFile(QStringLiteral("/\\package-lock.json")).statusCode, 404);
#endif
    // The application itself still is, the source map included.
    CHECK_EQ(handler.serveFile(QStringLiteral("/js/app.js")).statusCode, 200);
    CHECK_EQ(handler.serveFile(QStringLiteral("/js/app.js.map")).statusCode, 200);
    CHECK_EQ(handler.serveFile(QStringLiteral("/index.html")).statusCode, 200);

    SECTION("app manifest — each listed file has its size");

    // The bootstrap orders its downloads by these: the largest first.
    CHECK_EQ(handler.servedSize(QStringLiteral("/index.html")), qint64(15));
    CHECK_EQ(handler.servedSize(QStringLiteral("/js/app.js")), qint64(9));
    CHECK_EQ(handler.servedSize(QStringLiteral("/assets/logo.png")), qint64(4));
    for (const QString& p : files)
        CHECK(handler.servedSize(p) >= 0);
    CHECK_EQ(handler.servedSize(QStringLiteral("/js/missing.js")), qint64(-1));

    SECTION("app manifest — paths are request paths, and stable");

    // The browser feeds these straight back as request paths, so a missing
    // leading slash would turn every one of them into a relative fetch against
    // whatever the current URL happens to be.
    for (const QString& p : files)
        CHECK(p.startsWith(QLatin1Char('/')));

    // Sorted, so two runs of the same build produce the same document — which is
    // what lets anything downstream compare or cache it.
    QStringList sorted = files;
    sorted.sort();
    CHECK(files == sorted);

    SECTION("app manifest — a directory that is not there is empty, not a guess");

    const StaticFileHandler missing(tmp.path() + QStringLiteral("/does-not-exist/"),
                                    QStringLiteral("1.2.3"));
    CHECK(missing.listFiles().isEmpty());
}
