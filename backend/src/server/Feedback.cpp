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

#include "server/Feedback.h"
#include "common/Edition.h"
#include "common/Logger.h"
#include "network/AddressRace.h"
#include "network/RendezvousClient.h"
#include "network/SessionMetrics.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

namespace {

constexpr qint64 kHourMs = 3600 * 1000;
// A relay answer is a few hundred bytes; anything longer is not one.
constexpr qint64 kMaxReply = 16 * 1024;
// 2^34 tries: far past the 2^26 the relay may ever ask, so an honest
// difficulty always ends well before it — and a broken one cannot spin for ever.
constexpr qint64 kMaxTries = qint64(1) << 34;

// The relay's own rule (mw-feedback/discord.go, emailRe): ASCII only, nothing
// that could close a code block, mention anyone or reorder text.
const QRegularExpression& emailRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9-]{1,63}(\.[A-Za-z0-9-]{1,63})*\.[A-Za-z]{2,63}$)"));
    return re;
}

bool plainChallenge(const QByteArray& c)
{
    if (c.isEmpty() || c.size() > 128) return false;
    for (char ch : c)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_'))
            return false;
    return true;
}

// What the relay names a refused field with (validate() in main.go): short,
// lowercase, underscores. Anything else is reported as "invalid".
QString relayCode(const QByteArray& body)
{
    static const QRegularExpression re(QStringLiteral("^[a-z_]{1,32}$"));
    const QString code =
        QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString();
    return re.match(code).hasMatch() ? code : QStringLiteral("invalid");
}

} // namespace

Feedback::Feedback(QString version, bool devEdition, QObject* parent)
    : QObject(parent)
    , m_Dev(devEdition)
{
    // x.y.z only: CI's stamp (0.3.0.gb7c-dev) names a commit, and the edition
    // already says "dev".
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral(R"(^(\d{1,3}\.\d{1,3}\.\d{1,3}))")).match(version);
    if (m.hasMatch()) m_Version = m.captured(1);

    // The rendezvous' own address (stream.{domain}, or MW_RENDEZVOUS_URL in a
    // lab), and only over HTTPS. A LAN-only build never leaves the network.
    if (!mw::edition::lanOnly()) {
        const QString base = RendezvousClient::baseUrl();
        if (base.startsWith(QLatin1String("https://"))) m_Base = base;
    }
    m_Now = []() { return QDateTime::currentMSecsSinceEpoch(); };
    m_Delay = [this](int ms, std::function<void()> fn) {
        QTimer::singleShot(ms, this, std::move(fn));
    };
}

QJsonObject Feedback::attachment() const
{
    QJsonObject o;
    if (!m_Version.isEmpty()) o[QStringLiteral("version")] = m_Version;
    o[QStringLiteral("edition")] = m_Dev ? QStringLiteral("dev") : QStringLiteral("release");
    o[QStringLiteral("os")] = SessionMetrics::platformToken();
    return o;
}

QString Feedback::refusal(const Request& req)
{
    const QString message = req.message.trimmed();
    if (message.size() < kMinMessageChars) return QStringLiteral("message_short");
    if (message.size() > kMaxMessageUnits) return QStringLiteral("message_long");
    const QString email = req.email.trimmed();
    if (!email.isEmpty() && (email.size() > kMaxEmailLen || !emailRe().match(email).hasMatch()))
        return QStringLiteral("email_invalid");
    return {};
}

int Feedback::leadingZeroBits(const QByteArray& challenge, const QByteArray& counter)
{
    const QByteArray sum =
        QCryptographicHash::hash(challenge + ':' + counter, QCryptographicHash::Sha256);
    int n = 0;
    for (char c : sum) {
        const auto b = static_cast<unsigned char>(c);
        if (b == 0) {
            n += 8;
            continue;
        }
        for (unsigned char mask = 0x80; mask && !(b & mask); mask >>= 1)
            ++n;
        return n;
    }
    return n;
}

QByteArray Feedback::solve(const QByteArray& challenge, int bits)
{
    for (qint64 i = 0; i < kMaxTries; ++i) {
        const QByteArray counter = QByteArray::number(i, 36); // lowercase, as the relay allows
        if (leadingZeroBits(challenge, counter) >= bits) return counter;
    }
    return {};
}

QNetworkRequest Feedback::networkRequest(const QString& url)
{
    QNetworkRequest req{QUrl(url)};
    // The product, not the version: the version leaves only when its box is ticked.
    req.setRawHeader("User-Agent", "MoonlightWeb");
    req.setRawHeader("Accept", "application/json");
    // A redirect would be a way to have the message land elsewhere.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
    req.setTransferTimeout(kTimeoutMs);
    AddressRace::enableOn(req);
    return req;
}

QByteArray Feedback::payload(const Request& req, const QByteArray& challenge,
                             const QByteArray& pow) const
{
    QJsonObject o;
    o[QStringLiteral("challenge")] = QString::fromLatin1(challenge);
    o[QStringLiteral("pow")] = QString::fromLatin1(pow);
    o[QStringLiteral("message")] = req.message.trimmed();
    const QString email = req.email.trimmed();
    if (!email.isEmpty()) o[QStringLiteral("email")] = email;
    // The relay's hidden field, always empty from here: a filled one is
    // stopped at the route, before anything is sent.
    o[QStringLiteral("website")] = QString();
    if (req.attachVersion) o[QStringLiteral("meta")] = attachment();
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void Feedback::http(const QString& url, const QByteArray* body,
                    std::function<void(int, QByteArray)> reply)
{
    if (m_Transport) {
        m_Transport(url, body, std::move(reply));
        return;
    }
    if (!m_Nam) m_Nam = new QNetworkAccessManager(this);
    QNetworkRequest req = networkRequest(url);
    QNetworkReply* r = nullptr;
    if (body) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        r = m_Nam->post(req, *body);
    } else {
        r = m_Nam->get(req);
    }
    connect(r, &QNetworkReply::finished, r, [r, reply = std::move(reply)]() {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply(status, r->read(kMaxReply));
    });
}

void Feedback::finish(Outcome o, const QString& detail)
{
    m_Busy = false;
    Done done = std::move(m_Done);
    m_Done = nullptr;
    if (done) done(o, detail);
}

void Feedback::send(const Request& req, Done done)
{
    if (!available()) {
        done(Outcome::Unavailable, QStringLiteral("unavailable"));
        return;
    }
    if (const QString why = refusal(req); !why.isEmpty()) {
        done(Outcome::Invalid, why);
        return;
    }
    if (m_Busy) {
        done(Outcome::Busy, QStringLiteral("busy"));
        return;
    }
    const qint64 now = m_Now();
    m_Recent.removeIf([now](qint64 t) { return now - t >= kHourMs; });
    if (m_Recent.size() >= kMaxPerHour) {
        done(Outcome::RateLimited, QStringLiteral("rate_limited"));
        return;
    }
    m_Recent.append(now);
    m_Busy = true;
    m_Done = std::move(done);

    http(m_Base + QStringLiteral("/v1/feedback/challenge"), nullptr,
         [this, req](int status, QByteArray body) { challenged(status, body, req); });
}

void Feedback::challenged(int status, const QByteArray& body, const Request& req)
{
    if (status != 200) {
        Logger::info(QStringLiteral("[Feedback] not sent: challenge HTTP %1").arg(status));
        if (status == 0) return finish(Outcome::Unreachable, QStringLiteral("unreachable"));
        if (status == 429) return finish(Outcome::RateLimited, QStringLiteral("rate_limited"));
        return finish(Outcome::Unavailable, QStringLiteral("unavailable"));
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QByteArray challenge = o.value(QStringLiteral("challenge")).toString().toLatin1();
    const int bits = o.value(QStringLiteral("bits")).toInt(-1);
    const int minAge = o.value(QStringLiteral("minAgeSecs")).toInt(-1);
    if (!plainChallenge(challenge) || bits < 1 || bits > kMaxBits || minAge < 0 ||
        minAge > kMaxMinAgeSecs) {
        Logger::info(QStringLiteral("[Feedback] not sent: unexpected challenge"));
        return finish(Outcome::Unavailable, QStringLiteral("unavailable"));
    }
    const qint64 received = m_Now();

    // The work, then the wait: whatever the work took counts toward the wait.
    auto afterWork = [this, req, challenge, minAge, received](const QByteArray& pow) {
        if (pow.isEmpty()) return finish(Outcome::Unavailable, QStringLiteral("unavailable"));
        const qint64 waited = m_Now() - received;
        // One second more than asked: the relay counts whole seconds.
        const qint64 left = qMax<qint64>(0, (minAge + 1) * 1000LL - waited);
        const QByteArray body = payload(req, challenge, pow);
        m_Delay(int(left), [this, body]() {
            http(m_Base + QStringLiteral("/v1/feedback"), &body,
                 [this](int status, QByteArray reply) { posted(status, reply); });
        });
    };
    if (m_InlineWork) {
        afterWork(solve(challenge, bits));
        return;
    }
    QPointer<Feedback> self(this);
    QThreadPool::globalInstance()->start([self, challenge, bits, afterWork]() {
        const QByteArray pow = solve(challenge, bits);
        if (!self) return;
        QMetaObject::invokeMethod(
            self.data(), [afterWork, pow]() { afterWork(pow); }, Qt::QueuedConnection);
    });
}

void Feedback::posted(int status, const QByteArray& body)
{
    if (status == 200) {
        Logger::info(QStringLiteral("[Feedback] sent"));
        return finish(Outcome::Sent);
    }
    Logger::info(QStringLiteral("[Feedback] not sent: HTTP %1").arg(status));
    switch (status) {
    case 0: return finish(Outcome::Unreachable, QStringLiteral("unreachable"));
    case 400: return finish(Outcome::Invalid, relayCode(body));
    case 429: return finish(Outcome::RateLimited, QStringLiteral("rate_limited"));
    default: return finish(Outcome::Unavailable, QStringLiteral("unavailable"));
    }
}
