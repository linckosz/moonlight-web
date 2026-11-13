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

#include "backend/NvApp.h"

#include <QList>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

/*
 * Links straight to one app of one host (issue #63).
 *
 * A home-screen icon that opens a computer's Desktop in one tap, the way a
 * remote-desktop app does, needs an address that names the app. The app's own
 * names are poor at that: a Sunshine app's id comes from its name, so renaming
 * it changes both, and a host's address changes with the network. So the link
 * carries a short alias — `#go=<alias>`, in the fragment, which no browser sends
 * to a server — and this server keeps what the alias points at: the host by its
 * uuid, which no address change touches, and the app by its id, its last known
 * name, and for a display of the native host the key that names the screen.
 *
 * An alias is a pointer and nothing else. It opens no door: whoever follows it
 * still needs a session here and the right to launch on that host, exactly as a
 * click on the app's card does, and it never launches anything by itself — the
 * page does, through the same route the card uses.
 *
 * It follows the app on its own only where that cannot be wrong: the same id
 * (the host renamed the app and kept its id), the same name exactly (the host
 * gave it a new id), the same screen key (a display of the native host). When
 * one app vanished and one new one appeared in the same refresh, that is most
 * likely a rename, but not surely — it may be a removal and an unrelated
 * addition — so the link is only marked "to confirm", with the new app as its
 * candidate, and the person who opens it next is asked. Nothing else ever
 * moves an alias.
 *
 * Kept in app-links.json next to settings.json, written whole through
 * QSaveFile so the file never holds half a document, and read entry by entry so
 * one damaged entry costs that entry and not the others. A file that cannot be
 * read at all is never written over: the links it holds are not this server's
 * to throw away.
 */

namespace mw::applinks {

/// Characters in an alias.
constexpr int kAliasLength = 10;

/// Whether @p alias has an alias's shape: exactly ten ASCII letters and digits.
/// Checked before anything is looked up or written into an address.
inline bool validAlias(const QString& alias)
{
    if (alias.size() != kAliasLength) return false;
    for (const QChar c : alias) {
        const ushort u = c.unicode();
        const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
        if (!ok) return false;
    }
    return true;
}

/// A fresh alias, drawn from the system's random source (never from a hash of
/// what it points at: an alias must say nothing about its app). Never one of
/// lowercase letters alone: the server's log of paths answered on this
/// machine's own address keeps a lowercase word as it is and stars anything
/// else, and an alias must never be the word it keeps.
QString newAlias();

} // namespace mw::applinks

/// One link: what an alias points at.
struct AppLink
{
    enum class State
    {
        Present,   ///< the app was in the host's last list
        Missing,   ///< it was not, and nothing stood in for it
        ToConfirm, ///< it vanished as one new app appeared: ask before following
    };

    QString alias;
    QString hostUuid;
    int appId = 0;
    /// The app's last known name: what the link falls back on when the id
    /// changes, and what the page shows.
    QString name;
    /// A display of the native host: the key of the screen (NvApp::device()
    /// "key"), which outlives the display's app id. Empty for any other app.
    QString deviceKey;
    State state = State::Present;
    /// ToConfirm: the app that may be this one renamed.
    int candidateId = 0;
    QString candidateName;
    qint64 created = 0;      ///< seconds since the epoch
    qint64 missingSince = 0; ///< seconds since the epoch, 0 while not Missing
};

class AppLinkStore
{
public:
    /// The most links kept. Past it, a new one is refused rather than an old one
    /// dropped: a link on someone's home screen is not this server's to break.
    static constexpr int kMaxLinks = 500;

    /// @p filePath the file the links live in. @p aliasSource and @p clock are
    /// there for the tests; the defaults are mw::applinks::newAlias and the
    /// system clock.
    explicit AppLinkStore(const QString& filePath, std::function<QString()> aliasSource = nullptr,
                          std::function<qint64()> clock = nullptr);

    /// Reads the file. A missing file is an empty store; an unreadable one is
    /// an empty store that refuses to write (see writable()).
    void load();

    /// False when the file was there but could not be read: nothing is written
    /// over it, so every change is refused.
    bool writable() const { return !m_Unreadable; }

    enum class CreateResult
    {
        Created,
        Existing,   ///< the app already had a link: the same alias, not a second one
        Full,       ///< kMaxLinks reached
        Unwritable, ///< the change could not be kept
    };
    struct Created
    {
        CreateResult result = CreateResult::Unwritable;
        AppLink link;
    };

    /// The link to @p app of the host @p hostUuid, made if there is none. One
    /// link per app: a display of the native host is the same app whatever id
    /// it has today, so it is matched by its screen key.
    Created create(const QString& hostUuid, const NvApp& app);

    /// The link of @p alias, if there is one.
    std::optional<AppLink> find(const QString& alias) const;

    /// What the host's fresh, complete list of apps @p current says about its
    /// links. @p previous is the list it replaces (empty when none was known),
    /// which is what tells a rename from an app that merely went away. An empty
    /// @p current says nothing: a host that answered no apps at all is not a
    /// host whose every app was removed.
    void reconcile(const QString& hostUuid, const QVector<NvApp>& previous,
                   const QVector<NvApp>& current);

    enum class ConfirmResult
    {
        Followed,   ///< the link now points at its candidate
        NotPending, ///< nothing to confirm, or not that app
        Unknown,    ///< no such alias
        Unwritable, ///< the change could not be kept
    };

    /// The person opening the link said yes: the app @p candidateId is the one
    /// the link meant. Refused unless that is the candidate the link holds, and
    /// unless @p current (the host's list now) still has it.
    ConfirmResult confirm(const QString& alias, int candidateId, const QVector<NvApp>& current);

    /// Drops every link to the host @p hostUuid: removed from this server, or no
    /// longer paired with it. Returns how many went.
    int purgeHost(const QString& hostUuid);

    int size() const { return static_cast<int>(m_Links.size()); }

    /// The file's name inside the settings directory.
    static QString fileName();

    /// The state's word in the file and on the wire.
    static QString stateName(AppLink::State state);

private:
    bool save() const;
    qint64 now() const;
    QString freshAlias() const;

    QString m_FilePath;
    std::function<QString()> m_AliasSource;
    std::function<qint64()> m_Clock;
    QList<AppLink> m_Links;
    bool m_Unreadable = false;
};
