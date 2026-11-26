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

#include "server/ClientLogLines.h"
#include "server/LogScrubber.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

Logger::Level levelOf(const QString& l)
{
    if (l == QLatin1String("error")) return Logger::Error;
    if (l == QLatin1String("warn")) return Logger::Warning;
    if (l == QLatin1String("debug")) return Logger::Debug;
    return Logger::Info;
}

} // namespace

QList<ClientLogLine> clientLogLines(const QJsonObject& body, const QString& who,
                                    const QString& address)
{
    QList<ClientLogLine> out;

    // Who, in every line: the page's own id (two tabs are two pages), the kind
    // of user, and where from.
    static const QRegularExpression notId(QStringLiteral("[^A-Za-z0-9]"));
    QString id = body.value(QStringLiteral("id")).toString().left(16);
    id.remove(notId);
    const QString tag = QStringLiteral("[%1 %2@%3]").arg(id, who, address);

    // Share links and secrets are hidden before anything is cut to length, so
    // that no part of one is left at a cut.
    auto clean = [](const QString& text) {
        return neutralizeClientText(hideClientSecrets(LogScrubber::hideShareLinks(text)));
    };
    if (body.value(QStringLiteral("seq")).toInt() == 0) {
        const QString page = clean(body.value(QStringLiteral("page")).toString()).left(200);
        const QString ua = clean(body.value(QStringLiteral("ua")).toString()).left(300);
        out.append({Logger::Info, tag + QStringLiteral(" page %1 — %2").arg(page, ua)});
    }
    const int dropped = body.value(QStringLiteral("dropped")).toInt();
    if (dropped > 0)
        out.append({Logger::Warning,
                    tag + QStringLiteral(" %1 earlier lines dropped (the page's buffer was full)")
                              .arg(dropped)});

    for (const QJsonValue& v : body.value(QStringLiteral("lines")).toArray()) {
        const QJsonObject line = v.toObject();
        // The page's own clock: the server's stamp says when the batch came in,
        // this one when the line was written.
        const QString when =
            QDateTime::fromMSecsSinceEpoch(qint64(line.value(QStringLiteral("t")).toDouble()))
                .toString(QStringLiteral("hh:mm:ss.zzz"));
        QString text = clean(line.value(QStringLiteral("m")).toString());
        if (text.size() > kClientLineChars)
            text = text.left(kClientLineChars) + QStringLiteral(" […]");
        out.append({levelOf(line.value(QStringLiteral("l")).toString()),
                    tag + QLatin1Char(' ') + when + QLatin1Char(' ') + text});
    }
    return out;
}

bool clientLogHeard(bool debug, bool outOfDebug)
{
    return debug || outOfDebug;
}

QString neutralizeClientText(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QString out;
    out.reserve(text.size());
    for (const QChar c : std::as_const(text)) {
        const char16_t u = c.unicode();
        if (u == u'\n' || u == u'\r')
            out += QStringLiteral(" ⏎ ");
        else if (u == u'\t')
            out += QLatin1Char(' ');
        else if (u < 0x20 || (u >= 0x7f && u < 0xa0) || u == 0x2028 || u == 0x2029)
            out += QChar(0xfffd);
        else
            out += c;
    }
    return out;
}

QString hideClientSecrets(QString text)
{
    // A link's fragment keys (#k=, #n= and the same after an &), and the
    // parameters that carry a credential, wherever they appear: a URL, a JSON
    // dump, a form body. The value ends where a URL's or a quote's would.
    static const QRegularExpression param(
        QStringLiteral("([#?&;]\\s*(?:k|n|code|pin|key|token|password|passwd|pwd|session|"
                       "auth|sid)\\s*=\\s*)[^&#\\s\"'<>;,]+"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(param, QStringLiteral("\\1(hidden)"));
    // JSON fields of the same names, and a cookie's: "code":"123456", "pin":4829,
    // "k":"…" (an owner link's key, as an exchange's answer carries it).
    static const QRegularExpression field(
        QStringLiteral("(\"(?:k|n|code|pin|key|token|password|passwd|pwd|session|auth|sid|"
                       "cookie)\"\\s*:\\s*)(?:\"[^\"]*\"|[^,}\\]\\s]+)"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(field, QStringLiteral("\\1\"(hidden)\""));
    // Headers, to the end of their value.
    static const QRegularExpression header(
        QStringLiteral("\\b((?:set-)?cookie|authorization)(\\s*[:=]\\s*)[^\\n]*"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(header, QStringLiteral("\\1\\2(hidden)"));
    // Our own cookies by name, wherever a page prints one.
    static const QRegularExpression cookie(QStringLiteral("\\b(mw_[a-z_]+=)[^;\\s\"']+"),
                                           QRegularExpression::CaseInsensitiveOption);
    text.replace(cookie, QStringLiteral("\\1(hidden)"));
    return text;
}

QString clientLogSender(const QString& sessionToken, const QString& address)
{
    if (sessionToken.isEmpty()) return QStringLiteral("host@") + address;
    // A digest, cut: enough to tell sessions apart, nothing to sign in with.
    const QByteArray digest =
        QCryptographicHash::hash(sessionToken.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("session ") + QString::fromLatin1(digest.left(12));
}

QList<ClientLogLine> ClientLogBudget::admit(const QString& sender,
                                            const QList<ClientLogLine>& lines, const Limits& limits,
                                            qint64 nowMs)
{
    QMutexLocker lock(&m_Mutex);
    QList<ClientLogLine> out;

    auto it = m_Windows.find(sender);
    if (it == m_Windows.end()) {
        if (m_Windows.size() >= kMaxSenders) {
            // The stalest first: a sender that has not written for longest.
            auto stalest = m_Windows.begin();
            for (auto w = m_Windows.begin(); w != m_Windows.end(); ++w)
                if (w->start < stalest->start) stalest = w;
            m_Windows.erase(stalest);
        }
        it = m_Windows.insert(sender, Window{nowMs, 0, 0, 0});
    } else if (nowMs - it->start >= kWindowMs || nowMs < it->start) {
        if (it->dropped > 0)
            out.append({Logger::Warning,
                        QStringLiteral("[%1] %2 client lines dropped (over %3 lines or %4 bytes "
                                       "a minute)")
                            .arg(sender)
                            .arg(it->dropped)
                            .arg(limits.lines)
                            .arg(limits.bytes)});
        *it = Window{nowMs, 0, 0, 0};
    }

    for (const ClientLogLine& line : lines) {
        const qint64 size = line.text.toUtf8().size() + 1;
        if (it->lines + 1 > limits.lines || it->bytes + size > limits.bytes) {
            it->dropped++;
            continue;
        }
        it->lines++;
        it->bytes += size;
        out.append(line);
    }
    return out;
}
