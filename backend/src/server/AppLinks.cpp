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

#include "server/AppLinks.h"

#include "common/Logger.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>

#include <utility>

namespace mw::applinks {

QString newAlias()
{
    static const char kAlphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    constexpr quint32 kSize = sizeof(kAlphabet) - 1;
    static_assert(kSize == 62, "an alias is drawn from 62 characters");

    QRandomGenerator* rng = QRandomGenerator::system();
    for (;;) {
        QString alias;
        alias.reserve(kAliasLength);
        bool lowercaseOnly = true;
        for (int i = 0; i < kAliasLength; ++i) {
            const char c = kAlphabet[rng->bounded(kSize)];
            if (c < 'a' || c > 'z') lowercaseOnly = false;
            alias.append(QLatin1Char(c));
        }
        if (!lowercaseOnly) return alias;
    }
}

} // namespace mw::applinks

namespace {

/// What the log says of a host: enough to tell two apart, not its whole uuid.
QString shortHost(const QString& uuid)
{
    return uuid.left(8);
}

/// The screen a display of the native host shows, or empty for any other app.
QString screenKeyOf(const NvApp& app)
{
    return app.device().value(QStringLiteral("key")).toString();
}

const NvApp* appWithId(const QVector<NvApp>& apps, int id)
{
    for (const NvApp& app : apps)
        if (app.id() == id) return &app;
    return nullptr;
}

const NvApp* appWithScreenKey(const QVector<NvApp>& apps, const QString& key)
{
    if (key.isEmpty()) return nullptr;
    for (const NvApp& app : apps)
        if (screenKeyOf(app) == key) return &app;
    return nullptr;
}

/// The one app named exactly @p name. Two of that name are no answer: the link
/// would be guessing.
const NvApp* onlyAppNamed(const QVector<NvApp>& apps, const QString& name)
{
    if (name.isEmpty()) return nullptr;
    const NvApp* found = nullptr;
    for (const NvApp& app : apps) {
        if (app.name() != name) continue;
        if (found) return nullptr;
        found = &app;
    }
    return found;
}

std::optional<AppLink::State> stateFromName(const QString& word)
{
    if (word == QLatin1String("present")) return AppLink::State::Present;
    if (word == QLatin1String("missing")) return AppLink::State::Missing;
    if (word == QLatin1String("confirm")) return AppLink::State::ToConfirm;
    return std::nullopt;
}

QJsonObject toJson(const AppLink& link)
{
    QJsonObject obj;
    obj[QStringLiteral("alias")] = link.alias;
    obj[QStringLiteral("host")] = link.hostUuid;
    obj[QStringLiteral("app_id")] = link.appId;
    obj[QStringLiteral("name")] = link.name;
    if (!link.deviceKey.isEmpty()) obj[QStringLiteral("device_key")] = link.deviceKey;
    obj[QStringLiteral("state")] = AppLinkStore::stateName(link.state);
    if (link.state == AppLink::State::ToConfirm) {
        obj[QStringLiteral("candidate_id")] = link.candidateId;
        obj[QStringLiteral("candidate_name")] = link.candidateName;
    }
    obj[QStringLiteral("created")] = static_cast<double>(link.created);
    if (link.missingSince > 0)
        obj[QStringLiteral("missing_since")] = static_cast<double>(link.missingSince);
    return obj;
}

/// One entry of the file, or nothing when it is not one this server could have
/// written: then that entry alone is left out.
std::optional<AppLink> fromJson(const QJsonValue& value)
{
    if (!value.isObject()) return std::nullopt;
    const QJsonObject obj = value.toObject();

    AppLink link;
    link.alias = obj.value(QStringLiteral("alias")).toString();
    link.hostUuid = obj.value(QStringLiteral("host")).toString();
    link.appId = obj.value(QStringLiteral("app_id")).toInt(0);
    link.name = obj.value(QStringLiteral("name")).toString();
    link.deviceKey = obj.value(QStringLiteral("device_key")).toString();
    const auto state = stateFromName(obj.value(QStringLiteral("state")).toString());
    link.created = static_cast<qint64>(obj.value(QStringLiteral("created")).toDouble(0));
    link.missingSince = static_cast<qint64>(obj.value(QStringLiteral("missing_since")).toDouble(0));

    if (!mw::applinks::validAlias(link.alias) || link.hostUuid.isEmpty() || link.appId <= 0 ||
        !state)
        return std::nullopt;
    link.state = *state;

    if (link.state == AppLink::State::ToConfirm) {
        link.candidateId = obj.value(QStringLiteral("candidate_id")).toInt(0);
        link.candidateName = obj.value(QStringLiteral("candidate_name")).toString();
        // A question with nothing to ask about is no question: the link is
        // simply missing its app.
        if (link.candidateId <= 0) link.state = AppLink::State::Missing;
    }
    if (link.state != AppLink::State::Missing) link.missingSince = 0;
    return link;
}

} // namespace

AppLinkStore::AppLinkStore(const QString& filePath, std::function<QString()> aliasSource,
                           std::function<qint64()> clock)
    : m_FilePath(filePath)
    , m_AliasSource(std::move(aliasSource))
    , m_Clock(std::move(clock))
{}

QString AppLinkStore::fileName()
{
    return QStringLiteral("app-links.json");
}

QString AppLinkStore::stateName(AppLink::State state)
{
    switch (state) {
    case AppLink::State::Present: return QStringLiteral("present");
    case AppLink::State::Missing: return QStringLiteral("missing");
    case AppLink::State::ToConfirm: return QStringLiteral("confirm");
    }
    return QStringLiteral("missing");
}

qint64 AppLinkStore::now() const
{
    return m_Clock ? m_Clock() : QDateTime::currentSecsSinceEpoch();
}

QString AppLinkStore::freshAlias() const
{
    // An alias already in use is drawn again. With 62^10 of them that is a
    // formality, but a formality that keeps two links from ever sharing one.
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString alias = m_AliasSource ? m_AliasSource() : mw::applinks::newAlias();
        if (!mw::applinks::validAlias(alias)) continue;
        bool taken = false;
        for (const AppLink& link : m_Links)
            if (link.alias == alias) {
                taken = true;
                break;
            }
        if (!taken) return alias;
    }
    return QString();
}

void AppLinkStore::load()
{
    m_Links.clear();
    m_Unreadable = false;

    QFile file(m_FilePath);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) {
        m_Unreadable = true;
        Logger::warning(QStringLiteral("[AppLinks] Could not open %1: links are kept as they are, "
                                       "and no new one can be made")
                            .arg(m_FilePath));
        return;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    const QJsonValue links = doc.object().value(QStringLiteral("links"));
    if (error.error != QJsonParseError::NoError || !doc.isObject() || !links.isArray()) {
        m_Unreadable = true;
        Logger::warning(QStringLiteral("[AppLinks] %1 is not a links file: left as it is, and no "
                                       "new link can be made until it is")
                            .arg(m_FilePath));
        return;
    }

    int dropped = 0;
    QSet<QString> seen;
    for (const QJsonValue& value : links.toArray()) {
        const std::optional<AppLink> link = fromJson(value);
        if (!link || seen.contains(link->alias)) {
            ++dropped;
            continue;
        }
        seen.insert(link->alias);
        m_Links.append(*link);
    }
    if (dropped > 0)
        Logger::warning(
            QStringLiteral("[AppLinks] %1 damaged entr%2 of %3 left out")
                .arg(dropped)
                .arg(dropped == 1 ? QStringLiteral("y") : QStringLiteral("ies"), m_FilePath));
}

bool AppLinkStore::save() const
{
    if (m_Unreadable) return false;

    QJsonArray links;
    for (const AppLink& link : m_Links)
        links.append(toJson(link));
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("links")] = links;

    // QSaveFile, as settings.json: a sibling file renamed over this one, so the
    // path never holds half a document, whatever happens during the write.
    QSaveFile file(m_FilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::warning(QStringLiteral("[AppLinks] Failed to write %1").arg(m_FilePath));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        Logger::warning(QStringLiteral("[AppLinks] Failed to write %1: %2")
                            .arg(m_FilePath, file.errorString()));
        return false;
    }
    return true;
}

AppLinkStore::Created AppLinkStore::create(const QString& hostUuid, const NvApp& app)
{
    Created out;
    if (hostUuid.isEmpty() || app.id() <= 0) return out;
    if (m_Unreadable) return out;

    const QString screenKey = screenKeyOf(app);

    // One link per app. A display of the native host is matched by its screen,
    // whatever id it has today; any other app by its id.
    for (AppLink& link : m_Links) {
        if (link.hostUuid != hostUuid) continue;
        const bool same = screenKey.isEmpty() ? (link.deviceKey.isEmpty() && link.appId == app.id())
                                              : link.deviceKey == screenKey;
        if (!same) continue;

        // The person is looking at that app right now: whatever the link
        // thought before, it points at it again.
        const AppLink before = link;
        link.appId = app.id();
        link.name = app.name();
        link.state = AppLink::State::Present;
        link.candidateId = 0;
        link.candidateName.clear();
        link.missingSince = 0;
        const bool changed = before.appId != link.appId || before.name != link.name ||
                             before.state != link.state || before.candidateId != 0;
        if (changed && !save()) {
            link = before;
            return out;
        }
        out.result = CreateResult::Existing;
        out.link = link;
        return out;
    }

    if (m_Links.size() >= kMaxLinks) {
        out.result = CreateResult::Full;
        return out;
    }

    AppLink link;
    link.alias = freshAlias();
    if (link.alias.isEmpty()) return out;
    link.hostUuid = hostUuid;
    link.appId = app.id();
    link.name = app.name();
    link.deviceKey = screenKey;
    link.state = AppLink::State::Present;
    link.created = now();

    m_Links.append(link);
    if (!save()) {
        m_Links.removeLast();
        return out;
    }

    Logger::info(QStringLiteral("[AppLinks] Link made to app %1 of host %2")
                     .arg(link.appId)
                     .arg(shortHost(hostUuid)));
    out.result = CreateResult::Created;
    out.link = link;
    return out;
}

std::optional<AppLink> AppLinkStore::find(const QString& alias) const
{
    if (!mw::applinks::validAlias(alias)) return std::nullopt;
    for (const AppLink& link : m_Links)
        if (link.alias == alias) return link;
    return std::nullopt;
}

void AppLinkStore::reconcile(const QString& hostUuid, const QVector<NvApp>& previous,
                             const QVector<NvApp>& current)
{
    if (current.isEmpty() || m_Unreadable) return;

    // The one rename this refresh can stand for: exactly one app gone and
    // exactly one come, by id. Anything else — two gone, none come — says
    // nothing about which became which.
    int goneId = 0;
    const NvApp* come = nullptr;
    if (!previous.isEmpty()) {
        int gone = 0;
        for (const NvApp& app : previous)
            if (!appWithId(current, app.id())) {
                ++gone;
                goneId = app.id();
            }
        int came = 0;
        for (const NvApp& app : current)
            if (!appWithId(previous, app.id())) {
                ++came;
                come = &app;
            }
        if (gone != 1 || came != 1) {
            goneId = 0;
            come = nullptr;
        }
    }

    const qint64 at = now();
    bool changed = false;
    for (AppLink& link : m_Links) {
        if (link.hostUuid != hostUuid) continue;
        const AppLink before = link;

        const auto pointAt = [&link](const NvApp& app) {
            link.appId = app.id();
            link.name = app.name();
            link.state = AppLink::State::Present;
            link.candidateId = 0;
            link.candidateName.clear();
            link.missingSince = 0;
        };
        const auto lose = [&link, at]() {
            if (link.state != AppLink::State::Missing) link.missingSince = at;
            link.state = AppLink::State::Missing;
            link.candidateId = 0;
            link.candidateName.clear();
        };

        if (!link.deviceKey.isEmpty()) {
            // A display of the native host: its screen, or nothing.
            if (const NvApp* app = appWithScreenKey(current, link.deviceKey))
                pointAt(*app);
            else
                lose();
        } else if (const NvApp* sameId = appWithId(current, link.appId)) {
            pointAt(*sameId);
        } else if (const NvApp* sameName = onlyAppNamed(current, link.name)) {
            pointAt(*sameName);
        } else if (come && link.appId == goneId) {
            // Probably the same app renamed: asked, never assumed.
            link.state = AppLink::State::ToConfirm;
            link.candidateId = come->id();
            link.candidateName = come->name();
            link.missingSince = 0;
        } else if (link.state == AppLink::State::ToConfirm &&
                   appWithId(current, link.candidateId)) {
            // Still waiting for its answer, and the candidate is still there.
            if (const NvApp* candidate = appWithId(current, link.candidateId))
                link.candidateName = candidate->name();
        } else {
            lose();
        }

        if (link.appId != before.appId || link.name != before.name || link.state != before.state ||
            link.candidateId != before.candidateId || link.candidateName != before.candidateName ||
            link.missingSince != before.missingSince)
            changed = true;
    }

    if (changed) save();
}

AppLinkStore::ConfirmResult AppLinkStore::confirm(const QString& alias, int candidateId,
                                                  const QVector<NvApp>& current)
{
    if (!mw::applinks::validAlias(alias)) return ConfirmResult::Unknown;
    for (AppLink& link : m_Links) {
        if (link.alias != alias) continue;
        if (link.state != AppLink::State::ToConfirm || candidateId <= 0 ||
            link.candidateId != candidateId)
            return ConfirmResult::NotPending;
        const NvApp* candidate = appWithId(current, candidateId);
        if (!candidate) return ConfirmResult::NotPending;

        const AppLink before = link;
        link.appId = candidate->id();
        link.name = candidate->name();
        link.state = AppLink::State::Present;
        link.candidateId = 0;
        link.candidateName.clear();
        link.missingSince = 0;
        if (!save()) {
            link = before;
            return ConfirmResult::Unwritable;
        }
        return ConfirmResult::Followed;
    }
    return ConfirmResult::Unknown;
}

int AppLinkStore::purgeHost(const QString& hostUuid)
{
    if (hostUuid.isEmpty()) return 0;
    const QList<AppLink> before = m_Links;
    const qsizetype removed =
        m_Links.removeIf([&hostUuid](const AppLink& link) { return link.hostUuid == hostUuid; });
    if (removed == 0) return 0;
    if (!save()) {
        // Not kept on disk: kept here either, so what this server says and what
        // it will read back after a restart are the same.
        m_Links = before;
        return 0;
    }
    Logger::info(QStringLiteral("[AppLinks] %1 link%2 dropped with host %3")
                     .arg(removed)
                     .arg(removed == 1 ? QString() : QStringLiteral("s"), shortHost(hostUuid)));
    return static_cast<int>(removed);
}
