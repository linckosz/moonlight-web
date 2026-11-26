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

#include "test_framework.h"
#include "common/Logger.h"
#include "network/SessionMetrics.h"
#include "server/Feedback.h"
#include "server/routes/FeedbackRoutes.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

const qint64 kNow = 1'800'000'000'000;
const QString kBase = QStringLiteral("https://stream.example.test");
const QString kMessage = QStringLiteral("The stream stutters when I switch apps.");
const QString kEmail = QStringLiteral("player@example.org");

// The relay, as a script: what it answers to the challenge and to the post.
struct Harness
{
    qint64 clock = kNow;
    QList<int> delays;
    QStringList urls;
    QList<QByteArray> posts;
    int challengeStatus = 200;
    QByteArray challengeBody =
        R"({"challenge":"AQAAAABpJxdwtestchallenge","bits":8,"minAgeSecs":5})";
    int postStatus = 200;
    QByteArray postBody = R"({"ok":true})";
    bool hold = false; // never answer: the exchange stays in flight
    Feedback fb{QStringLiteral("0.4.4.gb7c-dev"), false};

    Harness()
    {
        fb.setEndpointForTests(kBase);
        fb.setClockForTests([this]() { return clock; });
        fb.setInlineWorkForTests(true);
        fb.setDelayForTests([this](int ms, std::function<void()> fn) {
            delays << ms;
            clock += ms;
            fn();
        });
        fb.setTransportForTests([this](const QString& url, const QByteArray* body,
                                       std::function<void(int, QByteArray)> reply) {
            urls << url;
            if (hold) return;
            if (body) {
                posts << *body;
                reply(postStatus, postBody);
            } else {
                reply(challengeStatus, challengeBody);
            }
        });
    }

    QPair<Feedback::Outcome, QString> send(const QString& message, const QString& email = {},
                                           bool attach = false)
    {
        QPair<Feedback::Outcome, QString> out{Feedback::Outcome::Unavailable,
                                              QStringLiteral("none")};
        fb.send({message, email, attach},
                [&out](Feedback::Outcome o, const QString& d) { out = {o, d}; });
        return out;
    }
};

HttpRequest ownerPost(const QByteArray& body, bool owner = true)
{
    HttpRequest req;
    req.method = QStringLiteral("POST");
    req.path = QStringLiteral("/api/feedback");
    req.body = body;
    req.isLocal = owner;
    return req;
}

} // namespace

void run_feedback_tests()
{
    SECTION("Feedback");

    // ── The relay's checks, made here first ─────────────────────────────────
    {
        CHECK_EQ(Feedback::refusal({QStringLiteral("  too short "), {}, false}),
                 QStringLiteral("message_short"));
        CHECK(Feedback::refusal({QString(1600, QLatin1Char('a')), {}, false}).isEmpty());
        CHECK_EQ(Feedback::refusal({QString(1601, QLatin1Char('a')), {}, false}),
                 QStringLiteral("message_long"));
        // Counted in UTF-16 units, as Discord and the relay count: 800 emoji fill it.
        QString emoji;
        for (int i = 0; i < 800; ++i)
            emoji += QString::fromUtf8("\xF0\x9F\x98\x80");
        CHECK(Feedback::refusal({emoji, {}, false}).isEmpty());
        CHECK_EQ(Feedback::refusal({emoji + QLatin1Char('a'), {}, false}),
                 QStringLiteral("message_long"));
        CHECK(Feedback::refusal(
                  {kMessage, QStringLiteral(" First.Last+mw@sub.example.co.uk "), false})
                  .isEmpty());
        for (const char* e : {"@everyone", "a`b@example.org", "a@b", "a b@example.org",
                              "<@1>@example.org", "a@example.org\n```"})
            CHECK_EQ(Feedback::refusal({kMessage, QString::fromUtf8(e), false}),
                     QStringLiteral("email_invalid"));
    }

    // ── The proof of work matches the relay's (values from its algorithm) ──
    {
        const QByteArray c = "AQAAAABpJxdwtestchallenge";
        CHECK_EQ(Feedback::solve(c, 8), QByteArray("ik"));
        CHECK_EQ(Feedback::solve(c, 12), QByteArray("1g3"));
        CHECK(Feedback::leadingZeroBits(c, "ik") >= 8);
        CHECK_EQ(Feedback::leadingZeroBits("x", "0"), 0);
    }

    // ── F2: what the request carries ───────────────────────────────────────
    {
        const QNetworkRequest r = Feedback::networkRequest(kBase + QStringLiteral("/v1/feedback"));
        CHECK_EQ(r.rawHeader("User-Agent"), QByteArray("MoonlightWeb"));
        CHECK_EQ(r.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
        CHECK_EQ(r.transferTimeout(), Feedback::kTimeoutMs);
        // Only these headers, and none of them names a version or the machine.
        for (const QByteArray& h : r.rawHeaderList()) {
            // Qt 6.7+ keeps header names lowercase (QHttpHeaders).
            CHECK(h.toLower() == "user-agent" || h.toLower() == "accept");
            CHECK(!r.rawHeader(h).contains("0.4"));
        }
    }

    // ── A full send: the box unticked sends no version, anywhere ────────────
    {
        QStringList logged;
        Logger::instance()->setTap(
            [&logged](Logger::Level, const QString& line) { logged << line; });
        Harness h;
        const auto out = h.send(kMessage, kEmail, false);
        Logger::instance()->setTap({});
        CHECK(out.first == Feedback::Outcome::Sent);
        CHECK_EQ(h.urls, (QStringList{kBase + QStringLiteral("/v1/feedback/challenge"),
                                      kBase + QStringLiteral("/v1/feedback")}));
        // The wait: one second past the relay's minimum, from the challenge.
        CHECK_EQ(h.delays, (QList<int>{6000}));
        CHECK_EQ(h.posts.size(), 1);
        const QJsonObject p = QJsonDocument::fromJson(h.posts.value(0)).object();
        QStringList keys = p.keys();
        keys.sort();
        CHECK_EQ(keys, (QStringList{QStringLiteral("challenge"), QStringLiteral("email"),
                                    QStringLiteral("message"), QStringLiteral("pow"),
                                    QStringLiteral("website")}));
        CHECK_EQ(p.value(QStringLiteral("pow")).toString(), QStringLiteral("ik"));
        CHECK_EQ(p.value(QStringLiteral("message")).toString(), kMessage);
        CHECK_EQ(p.value(QStringLiteral("website")).toString(), QString());
        CHECK(!h.posts.value(0).contains("0.4.4"));
        CHECK(!h.posts.value(0).contains(SessionMetrics::platformToken().toUtf8()));
        CHECK(!h.posts.value(0).contains("release"));
        // F3: the log says it left, and nothing of what left.
        CHECK(logged.join(QLatin1Char('\n')).contains(QStringLiteral("[Feedback] sent")));
        for (const QString& line : logged) {
            CHECK(!line.contains(QStringLiteral("stutters")));
            CHECK(!line.contains(QStringLiteral("player@")));
            CHECK(!line.contains(QStringLiteral("AQAAAAB")));
        }
    }

    // ── The box ticked: exactly attachment(), in the body ───────────────────
    {
        Harness h;
        CHECK(h.send(kMessage, {}, true).first == Feedback::Outcome::Sent);
        const QJsonObject p = QJsonDocument::fromJson(h.posts.value(0)).object();
        CHECK(!p.contains(QStringLiteral("email")));
        const QJsonObject meta = p.value(QStringLiteral("meta")).toObject();
        CHECK_EQ(meta, h.fb.attachment());
        // x.y.z only: CI's stamp stays home.
        CHECK_EQ(meta.value(QStringLiteral("version")).toString(), QStringLiteral("0.4.4"));
        CHECK_EQ(meta.value(QStringLiteral("edition")).toString(), QStringLiteral("release"));
        CHECK_EQ(meta.keys().size(), 3);
    }

    // ── F1: three an hour from this machine, one at a time ──────────────────
    {
        Harness h;
        for (int i = 0; i < Feedback::kMaxPerHour; ++i)
            CHECK(h.send(kMessage).first == Feedback::Outcome::Sent);
        const int calls = h.urls.size();
        CHECK(h.send(kMessage).first == Feedback::Outcome::RateLimited);
        CHECK_EQ(h.urls.size(), calls); // refused before asking the relay
        h.clock += 3600 * 1000;
        CHECK(h.send(kMessage).first == Feedback::Outcome::Sent);

        Harness held;
        held.hold = true;
        const auto first = held.send(kMessage);
        CHECK_EQ(first.second, QStringLiteral("none")); // still in flight
        CHECK(held.send(kMessage).first == Feedback::Outcome::Busy);
    }

    // ── What the relay answers, as the route will say it ────────────────────
    {
        Harness off;
        off.fb.setEndpointForTests(QString());
        CHECK(off.send(kMessage).first == Feedback::Outcome::Unavailable);
        CHECK(off.urls.isEmpty());

        Harness invalid;
        CHECK(invalid.send(QStringLiteral("short")).first == Feedback::Outcome::Invalid);
        CHECK(invalid.urls.isEmpty()); // no challenge spent on it

        Harness down;
        down.challengeStatus = 503;
        CHECK(down.send(kMessage).first == Feedback::Outcome::Unavailable);
        CHECK(down.posts.isEmpty());

        Harness greedy; // a difficulty past what this side will spend
        greedy.challengeBody = R"({"challenge":"abc","bits":40,"minAgeSecs":5})";
        CHECK(greedy.send(kMessage).first == Feedback::Outcome::Unavailable);
        CHECK(greedy.posts.isEmpty());

        Harness odd; // a challenge that is not plain base64url
        odd.challengeBody = R"({"challenge":"a\"b","bits":8,"minAgeSecs":5})";
        CHECK(odd.send(kMessage).first == Feedback::Outcome::Unavailable);

        Harness refused;
        refused.postStatus = 400;
        refused.postBody = R"({"error":"email_invalid"})";
        const auto r = refused.send(kMessage);
        CHECK(r.first == Feedback::Outcome::Invalid);
        CHECK_EQ(r.second, QStringLiteral("email_invalid"));

        Harness strange;
        strange.postStatus = 400;
        strange.postBody = R"({"error":"<b>Not a code</b>"})";
        CHECK_EQ(strange.send(kMessage).second, QStringLiteral("invalid"));

        Harness busy;
        busy.postStatus = 429;
        CHECK(busy.send(kMessage).first == Feedback::Outcome::RateLimited);

        Harness gone;
        gone.postStatus = 0;
        CHECK(gone.send(kMessage).first == Feedback::Outcome::Unreachable);

        Harness redirected; // not followed: a 302 is just an answer that is not 200
        redirected.postStatus = 302;
        CHECK(redirected.send(kMessage).first == Feedback::Outcome::Unavailable);
    }

    // ── The route: the owner's alone, the hidden field stops it here ────────
    {
        Harness h;
        int status = 0;
        QByteArray body;
        auto capture = [&status, &body](HttpResponse r) {
            status = r.statusCode;
            body = r.body;
        };
        const QByteArray ok =
            QJsonDocument(QJsonObject{{QStringLiteral("message"), kMessage}}).toJson();

        takeFeedback(ownerPost(ok, false), h.fb, capture);
        CHECK_EQ(status, 403);
        CHECK(h.urls.isEmpty());
        HttpRequest guestGet = ownerPost({}, false);
        guestGet.method = QStringLiteral("GET");
        CHECK_EQ(feedbackStatus(guestGet, h.fb).statusCode, 403);

        takeFeedback(ownerPost(QJsonDocument(QJsonObject{{QStringLiteral("message"), kMessage},
                                                         {QStringLiteral("website"),
                                                          QStringLiteral("http://spam")}})
                                   .toJson()),
                     h.fb, capture);
        CHECK_EQ(status, 200);
        CHECK(h.urls.isEmpty());

        takeFeedback(ownerPost("[1,2]"), h.fb, capture);
        CHECK_EQ(status, 400);
        takeFeedback(ownerPost(QByteArray(9 * 1024, 'a')), h.fb, capture);
        CHECK_EQ(status, 413);

        takeFeedback(ownerPost(ok), h.fb, capture);
        CHECK_EQ(status, 200);
        CHECK_EQ(h.posts.size(), 1);

        HttpRequest get = ownerPost({});
        get.method = QStringLiteral("GET");
        const HttpResponse st = feedbackStatus(get, h.fb);
        const QJsonObject o = QJsonDocument::fromJson(st.body).object();
        CHECK_EQ(st.statusCode, 200);
        CHECK(o.value(QStringLiteral("available")).toBool());
        CHECK_EQ(o.value(QStringLiteral("max_message")).toInt(), Feedback::kMaxMessageUnits);
        CHECK_EQ(o.value(QStringLiteral("attachment")).toObject(), h.fb.attachment());
    }
}
