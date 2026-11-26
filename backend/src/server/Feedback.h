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

// "Send feedback": the owner's message, from the server settings to the
// moderators' private Discord channel, through our relay
// (deploy/powerdns/mw-feedback). The webhook never comes near this process: it
// lives in the relay alone, and this side only knows the relay's address.
//
// What leaves, and nothing else
// -----------------------------
// The message, the e-mail address if one was typed, and — only when the box is
// ticked — the version, the edition and the OS family (attachment()). No
// rendezvous identifier, installation label, machine or user name, key or
// cookie; the product name alone identifies the client, so an unticked box
// really means no version. TLS is verified as for every other outbound call:
// nothing here ignores an SSL error.
//
// What is written down
// --------------------
// "[Feedback] sent", or the HTTP status / network error code. Never the message
// or the e-mail, at any level: they are not in a single log call of this file.
//
// The exchange
// ------------
// GET /v1/feedback/challenge, wait until the challenge is old enough (the relay
// refuses one younger than minAgeSecs), find the proof of work off the event
// loop, POST. One at a time, and kMaxPerHour an hour from this machine, so a
// page that loops cannot keep the CPU or the relay busy.

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

class Feedback : public QObject
{
    Q_OBJECT

public:
    /// What the relay accepts (mw-feedback/discord.go); checked here first so
    /// that a message refused by its length does not spend a challenge.
    static constexpr int kMaxMessageUnits = 1600; // UTF-16 units, as QString::size()
    static constexpr int kMinMessageChars = 10;
    static constexpr int kMaxEmailLen = 254;
    static constexpr int kMaxPerHour = 3;
    static constexpr int kTimeoutMs = 15000;
    /// The relay's own bounds are 8..26 bits and 5 s; past these, the answer
    /// is not one this side will spend a CPU or a wait on.
    static constexpr int kMaxBits = 26;
    static constexpr int kMaxMinAgeSecs = 30;

    enum class Outcome
    {
        Sent,
        Unavailable, // relay off, Discord busy, LAN-only build, no domain
        RateLimited, // here (kMaxPerHour) or at the relay
        Busy,        // one is already on its way
        Invalid,     // message or e-mail refused (detail names which)
        Unreachable, // no answer from the relay
    };

    struct Request
    {
        QString message;
        QString email;
        bool attachVersion = false;
    };

    using Done = std::function<void(Outcome, const QString& detail)>;

    /// @p version is MW_VERSION, whatever suffix CI stamped on it.
    Feedback(QString version, bool devEdition, QObject* parent = nullptr);

    /// The relay's base address, empty when feedback cannot leave this machine.
    QString endpoint() const { return m_Base; }
    bool available() const { return !m_Base.isEmpty(); }

    /// Exactly what the box adds when ticked — what the page shows beside it.
    QJsonObject attachment() const;

    /// Validates, then runs the exchange; @p done is called once, on this
    /// object's thread.
    void send(const Request& req, Done done);

    /// The checks the relay will make, in the same order. Empty = acceptable.
    static QString refusal(const Request& req);
    /// What the relay searches for: the leading zero bits of
    /// SHA-256("challenge:counter").
    static int leadingZeroBits(const QByteArray& challenge, const QByteArray& counter);
    /// The first counter, base 36, that reaches @p bits. Empty if none below
    /// a bound far past any honest difficulty.
    static QByteArray solve(const QByteArray& challenge, int bits);
    /// The request both exchanges use: verified TLS (nothing ignores an SSL
    /// error), no redirect followed, a bounded wait, the product's name alone.
    static QNetworkRequest networkRequest(const QString& url);
    /// The JSON posted to the relay for @p req.
    QByteArray payload(const Request& req, const QByteArray& challenge,
                       const QByteArray& pow) const;

    // ── Seams for the tests ─────────────────────────────────────────────────
    /// One HTTP exchange: GET when @p body is null. Calls back with the status
    /// (0 = no answer) and the body.
    using Transport = std::function<void(const QString& url, const QByteArray* body,
                                         std::function<void(int, QByteArray)> reply)>;
    void setTransportForTests(Transport t) { m_Transport = std::move(t); }
    void setClockForTests(std::function<qint64()> now) { m_Now = std::move(now); }
    /// Runs @p fn after @p ms (default: a single-shot timer).
    void setDelayForTests(std::function<void(int ms, std::function<void()> fn)> d)
    {
        m_Delay = std::move(d);
    }
    /// Runs the proof of work inline instead of on the thread pool.
    void setInlineWorkForTests(bool on) { m_InlineWork = on; }
    void setEndpointForTests(const QString& base) { m_Base = base; }

private:
    void http(const QString& url, const QByteArray* body,
              std::function<void(int, QByteArray)> reply);
    void finish(Outcome o, const QString& detail = {});
    void challenged(int status, const QByteArray& body, const Request& req);
    void posted(int status, const QByteArray& body);

    QString m_Base;
    QString m_Version; // x.y.z only
    bool m_Dev = false;
    QNetworkAccessManager* m_Nam = nullptr;
    Transport m_Transport;
    std::function<qint64()> m_Now;
    std::function<void(int, std::function<void()>)> m_Delay;
    bool m_InlineWork = false;

    QList<qint64> m_Recent; // when each of this hour's attempts started
    bool m_Busy = false;
    Done m_Done;
};
