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

#include "server/ErrorReports.h"
#include "common/Edition.h"
#include "network/AddressRace.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace {

constexpr qint64 kMaxAgoSecs = 7 * 24 * 3600;
/// Lines the tap compares a new one with before keeping it as a line of its own.
constexpr int kDedupWindow = 8;
/// What the host's lines may take of a report; the pages have the rest.
constexpr qsizetype kHostShare = 32 * 1024;

// The rule SessionMetrics::buildEndpoint() follows, for the same reasons: no
// report from a self-built binary, none with MW_NO_TELEMETRY, none from a build
// that must not reach the Internet.
QString endpoint()
{
    if (!qEnvironmentVariableIsEmpty("MW_NO_TELEMETRY") || mw::edition::lanOnly()) return {};
    const QString domain = QString::fromUtf8(qgetenv("MW_DOMAIN")).trimmed();
    const QString token = QString::fromUtf8(qgetenv("MW_PDNS_TOKEN")).trimmed();
    if (domain.isEmpty() || token.isEmpty()) return {};
    return QStringLiteral("https://metrics.%1/v1/errors").arg(domain);
}

// A line kept while it waits: trimmed, one line, and bounded before the
// scrubber ever sees it (the final cut comes after, in build()).
QString waitingText(QStringView text)
{
    QString s = text.trimmed().toString();
    if (s.size() > 2 * ErrorReports::kMaxLineChars) s.truncate(2 * ErrorReports::kMaxLineChars);
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return s;
}

} // namespace

ErrorReports::ErrorReports(QString version, bool enabled, NamesFn names, QObject* parent)
    : QObject(parent)
    , m_Version(std::move(version))
    , m_Names(std::move(names))
    , m_Url(endpoint())
    , m_Token(QString::fromUtf8(qgetenv("MW_PDNS_TOKEN")).trimmed())
    , m_Nam(new QNetworkAccessManager(this))
    , m_Timer(new QTimer(this))
{
    m_Enabled = enabled;
    m_Pool.setMaxThreadCount(1);
    m_Timer->setSingleShot(true);
    connect(m_Timer, &QTimer::timeout, this, &ErrorReports::fire);
}

ErrorReports::~ErrorReports()
{
    if (m_Attached) m_Attached->setTap({});
    m_Pool.waitForDone();
}

qint64 ErrorReports::now() const
{
    return m_Clock ? m_Clock() : QDateTime::currentMSecsSinceEpoch();
}

bool ErrorReports::active() const
{
    return m_Enabled && (!m_Url.isEmpty() || m_SendForTests);
}

void ErrorReports::setEnabled(bool enabled)
{
    m_Enabled = enabled;
    if (enabled) return;
    // Said no: what was waiting does not leave later either.
    m_Timer->stop();
    m_DueMs = 0;
    QMutexLocker lock(&m_Mutex);
    m_Host.clear();
    m_HostChars = 0;
    m_HostDropped = 0;
    m_Pages.clear();
}

void ErrorReports::attach(Logger* log)
{
    m_Attached = log;
    log->setTap([this](Logger::Level level, const QString& message) { tap(level, message); });
}

void ErrorReports::tap(Logger::Level level, const QString& message)
{
    if (!m_Enabled) return;
    bool error = false;
    QStringView text(message);
    if (level >= Logger::Warning) {
        error = level == Logger::Error;
    } else if (level == Logger::Info &&
               message.startsWith(QLatin1String("[StreamWorker/child] "))) {
        // A worker's line, relayed from its stderr as INFO: its own level is in
        // the text, "[StreamWorker/child] [2026-11-12 14:35:10.123] [ERROR] …".
        static const QLatin1String kError("] [ERROR] ");
        static const QLatin1String kWarn("] [WARN] ");
        qsizetype at = message.indexOf(kError);
        if (at >= 0 && at < 64) {
            error = true;
            text = text.mid(at + kError.size());
        } else if ((at = message.indexOf(kWarn)) >= 0 && at < 64) {
            text = text.mid(at + kWarn.size());
        } else {
            return;
        }
    } else {
        return;
    }

    const QString kept = waitingText(text);
    if (kept.isEmpty()) return;
    const qint64 at = now();

    QMutexLocker lock(&m_Mutex);
    // The same line again (a loop that fails every frame): one line, counted.
    for (qsizetype i = m_Host.size() - 1, seen = 0; i >= 0 && seen < kDedupWindow; --i, ++seen) {
        Line& l = m_Host[i];
        if (l.error == error && l.text == kept) {
            l.count++;
            l.atMs = at;
            return;
        }
    }
    m_Host.append({at, error, 1, kept});
    m_HostChars += kept.size();
    while (m_Host.size() > kMaxHostLines || m_HostChars > kMaxHostChars) {
        m_HostChars -= m_Host.first().text.size();
        m_Host.removeFirst();
        m_HostDropped++;
    }
}

void ErrorReports::addPage(const QString& who, const QString& userAgent, const QJsonObject& body)
{
    if (!m_Enabled) return;
    Page page;
    page.who = who;
    page.browser = browserFamily(userAgent, &page.major);
    page.dropped = qBound(0, body.value(QStringLiteral("dropped")).toInt(), 1000000);

    // The page's clock may be off: only the gap between a line and the send is
    // read from it, and placed on ours.
    const qint64 sent = qint64(body.value(QStringLiteral("sent")).toDouble());
    const qint64 at = now();
    const QJsonArray lines = body.value(QStringLiteral("lines")).toArray();
    const qsizetype first = std::max<qsizetype>(0, lines.size() - kMaxPageLines);
    page.dropped += int(first);
    for (qsizetype i = first; i < lines.size(); ++i) {
        const QJsonObject l = lines.at(i).toObject();
        const QString text = waitingText(l.value(QStringLiteral("m")).toString());
        if (text.isEmpty()) continue;
        const qint64 ago = qBound<qint64>(0, sent - qint64(l.value(QStringLiteral("t")).toDouble()),
                                          kMaxAgoSecs * 1000);
        page.lines.append(
            {at - ago, true, qBound(1, l.value(QStringLiteral("n")).toInt(1), 100000), text});
    }
    if (page.lines.isEmpty()) return;

    QMutexLocker lock(&m_Mutex);
    if (m_Pages.size() >= kMaxPages) m_Pages.removeFirst();
    m_Pages.append(page);
}

QJsonObject ErrorReports::headerFor(const QString& version, const SessionMetrics::Facts& f)
{
    return QJsonObject{
        {QStringLiteral("v"), version},
        {QStringLiteral("os"), SessionMetrics::platformToken()},
        {QStringLiteral("arch"), SessionMetrics::archToken()},
        {QStringLiteral("h"), f.height},
        {QStringLiteral("fps"), f.fps},
        {QStringLiteral("codec"), f.codec},
        {QStringLiteral("hdr"), f.hdr},
        {QStringLiteral("yuv444"), f.yuv444},
        {QStringLiteral("backend"), f.backend},
        {QStringLiteral("transport"), f.transport},
        {QStringLiteral("net"), f.net},
        {QStringLiteral("client"), f.client},
        {QStringLiteral("kind"), f.kind},
    };
}

void ErrorReports::streamEnded(const SessionMetrics::Facts& facts, int seconds)
{
    QJsonObject h = headerFor(m_Version, facts);
    h[QStringLiteral("event")] = QStringLiteral("end");
    h[QStringLiteral("seconds")] = seconds;
    arm(h);
}

void ErrorReports::streamFailed(const SessionMetrics::Facts& facts, int httpCode)
{
    QJsonObject h = headerFor(m_Version, facts);
    h[QStringLiteral("event")] = QStringLiteral("failed");
    h[QStringLiteral("code")] = httpCode;
    arm(h);
}

void ErrorReports::arm(const QJsonObject& header)
{
    // The latest stream is the one the report describes.
    m_Header = header;
    if (!active() || m_Timer->isActive()) return;
    const qint64 t = now();
    m_DueMs = std::max(t + kSettleMs, m_LastSentMs > 0 ? m_LastSentMs + kMinGapMs : 0);
    m_Timer->start(int(m_DueMs - t));
}

bool ErrorReports::take(Snapshot* out)
{
    QMutexLocker lock(&m_Mutex);
    const bool anyError = !m_Pages.isEmpty() || std::any_of(m_Host.cbegin(), m_Host.cend(),
                                                            [](const Line& l) { return l.error; });
    if (!anyError) return false;

    // Every ERROR, and the latest WARN only: the ring may hold days of them.
    int warnings = 0;
    QList<Line> host;
    for (qsizetype i = m_Host.size() - 1; i >= 0; --i) {
        const Line& l = m_Host.at(i);
        if (!l.error && ++warnings > kSentWarnings) continue;
        host.prepend(l);
    }
    out->header = m_Header;
    out->dropped = m_HostDropped + int(m_Host.size() - host.size());
    out->host = host;
    out->pages = m_Pages;
    m_Host.clear();
    m_HostChars = 0;
    m_HostDropped = 0;
    m_Pages.clear();
    return true;
}

void ErrorReports::fire()
{
    m_DueMs = 0;
    if (!active()) return;
    Snapshot snap;
    if (!take(&snap)) return;
    const qint64 t = now();
    m_LastSentMs = t;
    const LogScrubber::Names names = m_Names ? m_Names() : LogScrubber::Names();
    // The scrubber is a few hundred regular expressions over each line: never on
    // the HTTP thread, which is this one.
    m_Pool.start([this, snap, names, t]() {
        const QByteArray body = build(snap, names, t);
        QMetaObject::invokeMethod(this, [this, body]() { send(body); }, Qt::QueuedConnection);
    });
}

bool ErrorReports::flushForTests()
{
    if (!active()) return false;
    Snapshot snap;
    if (!take(&snap)) return false;
    const qint64 t = now();
    m_LastSentMs = t;
    m_Timer->stop();
    m_DueMs = 0;
    send(build(snap, m_Names ? m_Names() : LogScrubber::Names(), t));
    return true;
}

QByteArray ErrorReports::build(const Snapshot& snap, const LogScrubber::Names& names, qint64 now)
{
    // Which lines fit, newest first, on their size before scrubbing (stand-ins
    // are about as long as what they replace); the final cut comes after.
    int dropped = snap.dropped;
    qsizetype budget = kHostShare;
    qsizetype firstHost = snap.host.size();
    while (firstHost > 0 && budget - snap.host.at(firstHost - 1).text.size() >= 0) {
        budget -= snap.host.at(firstHost - 1).text.size() + 32;
        --firstHost;
    }
    dropped += int(firstHost);
    budget += kMaxReportChars - kHostShare;

    QList<qsizetype> firstLine(snap.pages.size(), 0);
    QList<int> pageDropped(snap.pages.size(), 0);
    for (qsizetype p = snap.pages.size() - 1; p >= 0; --p) {
        const QList<Line>& lines = snap.pages.at(p).lines;
        qsizetype i = lines.size();
        while (i > 0 && budget - lines.at(i - 1).text.size() >= 0) {
            budget -= lines.at(i - 1).text.size() + 32;
            --i;
        }
        firstLine[p] = i;
        pageDropped[p] = snap.pages.at(p).dropped + int(i);
    }

    // One scrubber for the whole report, oldest line first: a machine keeps one
    // letter throughout, in order of appearance, as in the archive.
    LogScrubber scrubber(names);
    auto lineJson = [&](const Line& l) {
        QString text = scrubber.scrubLine(l.text);
        if (text.size() > kMaxLineChars) text = text.left(kMaxLineChars) + QStringLiteral(" […]");
        QJsonObject o{
            {QStringLiteral("ago"), int(qBound<qint64>(0, (now - l.atMs) / 1000, kMaxAgoSecs))},
            {QStringLiteral("l"), l.error ? QStringLiteral("error") : QStringLiteral("warn")},
            {QStringLiteral("m"), text},
        };
        if (l.count > 1) o[QStringLiteral("n")] = l.count;
        return o;
    };

    QJsonObject body = snap.header;
    QJsonArray host;
    for (qsizetype i = firstHost; i < snap.host.size(); ++i)
        host.append(lineJson(snap.host.at(i)));
    body[QStringLiteral("host")] = host;
    body[QStringLiteral("dropped")] = dropped;
    QJsonArray pages;
    for (qsizetype p = 0; p < snap.pages.size(); ++p) {
        const Page& page = snap.pages.at(p);
        if (firstLine[p] >= page.lines.size()) continue;
        QJsonArray lines;
        for (qsizetype i = firstLine[p]; i < page.lines.size(); ++i)
            lines.append(lineJson(page.lines.at(i)));
        QJsonObject o{
            {QStringLiteral("who"), page.who},
            {QStringLiteral("browser"), page.browser},
            {QStringLiteral("major"), page.major},
            {QStringLiteral("lines"), lines},
        };
        if (pageDropped[p] > 0) o[QStringLiteral("dropped")] = pageDropped[p];
        pages.append(o);
    }
    body[QStringLiteral("pages")] = pages;
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void ErrorReports::send(const QByteArray& body)
{
    // Switched off while the report was being built on the pool: it stays.
    if (!m_Enabled) return;
    if (m_SendForTests) {
        m_SendForTests(body);
        return;
    }
    if (m_Url.isEmpty()) return;
    QNetworkRequest req{QUrl(m_Url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-API-Key", m_Token.toUtf8());
    // As the census: a redirect would be a way to have the report land elsewhere.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
    AddressRace::enableOn(req);
    QNetworkReply* reply = m_Nam->post(req, body);
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        reply->deleteLater();
        // DEBUG only: the tap reads WARN and up, and must not read its own
        // failure as an error to report.
        if (reply->error() != QNetworkReply::NoError)
            Logger::debug(
                QStringLiteral("[ErrorReports] report dropped: %1").arg(reply->errorString()));
    });
}

QString ErrorReports::browserFamily(const QString& userAgent, int* major)
{
    struct Rule
    {
        const char* family;
        QRegularExpression re;
    };
    // Order matters: Edge, Opera and Samsung also say Chrome, every iOS browser
    // says Safari, and an Android WebView says Chrome with "; wv)".
    static const QList<Rule> rules{
        {"samsung", QRegularExpression(QStringLiteral(R"(SamsungBrowser/(\d+))"))},
        {"edge", QRegularExpression(QStringLiteral(R"(Edg(?:e|A|iOS)?/(\d+))"))},
        {"opera", QRegularExpression(QStringLiteral(R"((?:OPR|OPT)/(\d+))"))},
        {"firefox", QRegularExpression(QStringLiteral(R"((?:Firefox|FxiOS)/(\d+))"))},
        {"chrome", QRegularExpression(QStringLiteral(R"(CriOS/(\d+))"))},
        {"webview", QRegularExpression(QStringLiteral(R"(; wv\).*Chrome/(\d+))"))},
        {"chrome", QRegularExpression(QStringLiteral(R"((?:Chrome|Chromium)/(\d+))"))},
        {"safari",
         QRegularExpression(QStringLiteral(R"(Version/(\d+)[^ ]* (?:Mobile/\S+ )?Safari/)"))},
    };
    for (const Rule& r : rules) {
        const QRegularExpressionMatch m = r.re.match(userAgent);
        if (!m.hasMatch()) continue;
        if (major) *major = qBound(0, m.captured(1).toInt(), 1000);
        return QString::fromLatin1(r.family);
    }
    if (major) *major = 0;
    return QStringLiteral("other");
}

void ErrorReports::setClockForTests(std::function<qint64()> clock)
{
    m_Clock = std::move(clock);
}

void ErrorReports::setSenderForTests(std::function<void(const QByteArray&)> send)
{
    m_SendForTests = std::move(send);
}

int ErrorReports::waitingHostLines() const
{
    QMutexLocker lock(&m_Mutex);
    return int(m_Host.size());
}
