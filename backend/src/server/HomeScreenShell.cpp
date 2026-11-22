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

#include "server/HomeScreenShell.h"

#include "server/AppLinks.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

namespace mw::homescreen {

namespace {

// The rendezvous identifier's shape (bootstrap/v1/tunnel.js ID_SHAPE). Anything
// else is not an address the entry page can open, so it is never written in.
const QRegularExpression kHostId(QStringLiteral("^[0-9a-z]{26}$"));

QString dataUrl(const QByteArray& png)
{
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

QString htmlAttribute(const QString& value)
{
    return value.toHtmlEscaped();
}

// Splices @p value over the first capture of the first match. Not
// QString::replace with "\1": the value is a name somebody typed, and a
// backslash in it would be read as a back-reference.
void replaceCaptured(QString& text, const QRegularExpression& pattern, const QString& value)
{
    const QRegularExpressionMatch m = pattern.match(text);
    if (m.hasMatch()) text.replace(m.capturedStart(1), m.capturedLength(1), value);
}

// What a name an icon is given may not hold, refused whole (security review,
// I63NAME C1/C9): the controls, what reorders or hides text, the line and
// paragraph separators, and U+FFFD, which is what an invalid UTF-8 or UTF-16
// sequence was decoded into.
bool refusedInName(char32_t c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x2028 && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF ||
           c == 0xFFFD;
}

} // namespace

QString appIconName(const QString& name)
{
    if (name.trimmed().isEmpty()) return {};
    const QList<uint> points = QStringView(name).toUcs4();
    if (points.size() > kAppIconNameMax) return {};
    for (const uint c : points)
        if (refusedInName(char32_t(c))) return {};
    return name;
}

QString appIconNameAsked(const QString& queryValue)
{
    if (QUrl::toPercentEncoding(queryValue).size() > kAppIconNameQueryMax) return {};
    return QUrl::fromPercentEncoding(queryValue.toUtf8());
}

QString title(const QString& editionName, const QString& machineName)
{
    const QString machine = machineName.trimmed();
    return machine.isEmpty() ? editionName : QStringLiteral("%1 [%2]").arg(editionName, machine);
}

QString shownMachineName(const QString& machineName, bool signedIn)
{
    return signedIn ? machineName : QString();
}

QString AppIconNames::shown() const
{
    const QString own = appIconName(shortName);
    return own.isEmpty() ? appIconName(name) : own;
}

AppIconNames appIconNamesAsked(const QString& n, bool nRepeated, const QString& s, bool sRepeated)
{
    if (QUrl::toPercentEncoding(n).size() + QUrl::toPercentEncoding(s).size() >
        kAppIconNamesQueryMax)
        return {};
    return {nRepeated ? QString() : appIconNameAsked(n),
            sRepeated ? QString() : appIconNameAsked(s)};
}

QByteArray manifest(const QByteArray& json, const QString& title, const QString& hostId,
                    const QString& handoffKey, const QByteArray& icon192, const QByteArray& icon512,
                    const QString& appLink)
{
    return manifest(json, title, hostId, handoffKey, icon192, icon512, appLink, AppIconNames());
}

QByteArray manifest(const QByteArray& json, const QString& title, const QString& hostId,
                    const QString& handoffKey, const QByteArray& icon192, const QByteArray& icon512,
                    const QString& appLink, const AppIconNames& names)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return json;

    QJsonObject obj = doc.object();
    if (!title.isEmpty()) {
        obj[QStringLiteral("name")] = title;
        obj[QStringLiteral("short_name")] = title;
    }
    // Ten letters and digits, nothing to encode: checked here as well as by the
    // caller, since it is written into an address — and encoded all the same,
    // so that what reaches the address is an address's even if the check moved.
    const QString go = mw::applinks::validAlias(appLink)
                           ? QString::fromLatin1(QUrl::toPercentEncoding(appLink))
                           : QString();
    const bool throughRendezvous = kHostId.match(hostId).hasMatch();
    if (throughRendezvous || !go.isEmpty()) {
        // The path form, the one people share: the entry page reads the machine
        // from it and needs nothing from storage. `id` follows, so two machines
        // installed side by side are two applications rather than one replaced.
        // Off the tunnel, this origin is the machine, and its "/" the address.
        //
        // The key goes in the fragment, which no browser sends to the server it
        // fetched from: the entry page forwards #k= to the application, and the
        // introduction server never sees it. `id` stays bare, so a new key is
        // the same application and not a second one. A link to one app goes in
        // the fragment for the same reason, and it does change `id` — in a
        // query, since an id drops its fragment: one shortcut per app.
        const QString address = throughRendezvous ? QLatin1Char('/') + hostId : QStringLiteral("/");
        QStringList fragment;
        if (!go.isEmpty()) fragment << QStringLiteral("go=") + go;
        if (throughRendezvous && !handoffKey.isEmpty())
            fragment << QStringLiteral("k=") +
                            QString::fromLatin1(QUrl::toPercentEncoding(handoffKey));
        obj[QStringLiteral("start_url")] =
            fragment.isEmpty() ? address
                               : address + QLatin1Char('#') + fragment.join(QLatin1Char('&'));
        obj[QStringLiteral("id")] = go.isEmpty() ? address : address + QStringLiteral("?go=") + go;
    }
    // A shortcut to one app names nothing of its own: not the app (the browser
    // fetches a manifest without the session, and nothing is looked up), not
    // the machine or the edition either. Only the names the page that made it
    // wrote into the request, given back as they were, each once it holds as a
    // name (appIconName): labels, and the alias alone says what opens. The
    // short name is the one a phone proposes for the icon: the name whole
    // (I63FULL), the phone cutting it under the icon itself; a short name of
    // its own only for an icon made before, which still asks for one.
    if (!go.isEmpty()) {
        obj.remove(QStringLiteral("name"));
        obj.remove(QStringLiteral("short_name"));
        const QString name = appIconName(names.name);
        const QString shortName = names.shown();
        if (!name.isEmpty()) obj[QStringLiteral("name")] = name;
        if (!shortName.isEmpty()) obj[QStringLiteral("short_name")] = shortName;
    }

    QJsonArray icons = obj.value(QStringLiteral("icons")).toArray();
    for (qsizetype i = 0; i < icons.size(); ++i) {
        QJsonObject icon = icons.at(i).toObject();
        const QString src = icon.value(QStringLiteral("src")).toString();
        const QByteArray& bytes = src.endsWith(QLatin1String("icon-192.png"))   ? icon192
                                  : src.endsWith(QLatin1String("icon-512.png")) ? icon512
                                                                                : QByteArray();
        if (bytes.isEmpty()) continue;
        icon[QStringLiteral("src")] = dataUrl(bytes);
        icons[i] = icon;
    }
    if (!icons.isEmpty()) obj[QStringLiteral("icons")] = icons;

    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

QByteArray shell(const QByteArray& html, const QString& title, const QByteArray& icon180)
{
    QString page = QString::fromUtf8(html);

    if (!title.isEmpty()) {
        static const QRegularExpression appTitle(QString::fromLatin1(
            R"re(<meta\s+name="apple-mobile-web-app-title"\s+content="([^"]*)")re"));
        replaceCaptured(page, appTitle, htmlAttribute(title));
    }

    if (!icon180.isEmpty()) {
        static const QRegularExpression touchIcon(
            QString::fromLatin1(R"re(<link\s+rel="apple-touch-icon"[^>]*\shref="([^"]*)")re"));
        replaceCaptured(page, touchIcon, dataUrl(icon180));
    }

    return page.toUtf8();
}

AppIconPart appIconPart(const QString& path, const QString& icon, const QString& go,
                        IconReach reach)
{
    // An allowlist of one: anything but the direct way in gets the ordinary files.
    if (reach != IconReach::DirectToHost) return AppIconPart::None;
    if ((path == QLatin1String("/") || path == QLatin1String("/index.html")) &&
        mw::applinks::validAlias(icon))
        return AppIconPart::Shell;
    if (path == QLatin1String("/manifest.webmanifest") && mw::applinks::validAlias(go))
        return AppIconPart::Manifest;
    return AppIconPart::None;
}

QByteArray appIconShell(const QByteArray& html, const QString& alias, const AppIconNames& names)
{
    if (!mw::applinks::validAlias(alias)) return html;
    static const QRegularExpression manifestLink(
        QString::fromLatin1(R"re(<link\s+rel="manifest"\s+href="([^"]*)")re"));
    QString page = QString::fromUtf8(html);
    // An attribute, so escaped as one, and its value an address, so encoded as
    // one — ten letters and digits come out of both unchanged. The names, each
    // when it holds as one, only ever here: encoded down to A-Z a-z 0-9 - . _ ~,
    // which leaves nothing for the HTML to read (security review, I63NAME C7).
    QString href = QStringLiteral("/manifest.webmanifest?go=") +
                   QString::fromLatin1(QUrl::toPercentEncoding(alias));
    const QString name = appIconName(names.name);
    const QString shortName = appIconName(names.shortName);
    if (!name.isEmpty())
        href += QStringLiteral("&n=") + QString::fromLatin1(QUrl::toPercentEncoding(name));
    if (!shortName.isEmpty())
        href += QStringLiteral("&s=") + QString::fromLatin1(QUrl::toPercentEncoding(shortName));
    replaceCaptured(page, manifestLink, htmlAttribute(href));
    return page.toUtf8();
}

} // namespace mw::homescreen
