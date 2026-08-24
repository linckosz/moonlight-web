/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Native co-op on Wolf: finding our own session so we can reap it, and the exact
 * wire shapes those calls take. The join that once lived here is gone — a guest
 * joins the owner's lobby natively, from the host's own streamed UI (Wolf UI) —
 * so what remains is the session-id resolution the reaping depends on, plus the
 * transport shapes kept as a validated reference.
 *
 * Everything here is pinned against Wolf's own source rather than against a
 * capture, because the failure mode is silent in both directions:
 *
 *  - Wolf parses request bodies with reflect-cpp against its C++ structs. A
 *    field of the wrong JSON type is not ignored, it makes the whole body
 *    unreadable and comes back as a generic 500 "Invalid event" that names
 *    nothing. The PIN is the trap: it is a std::vector<short> — an array of
 *    digits — and sending the string a human typed fails every time.
 *  - The session id is a std::size_t, but its parser is specialised to read
 *    from a JSON *string* (there is no unsigned 64-bit integer in JSON). So the
 *    field that looks numeric must be sent as text, and vice versa.
 *
 * The match rule is pinned for a different reason: it encodes a fact about Wolf
 * not visible from our side at all — that a session is identified by the launch
 * key it was created with.
 */
#include "test_framework.h"

#include "../src/backend/WolfApiClient.h"
#include "../src/backend/streambackend/WolfCoop.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

/// GET /api/v1/sessions as Wolf reflects it (events/reflectors.hpp): the session
/// id is published under `client_id`, and `aes_key` is the launch `rikey` stored
/// verbatim. Two rows on purpose — the point is telling ours from someone else's.
const char* const kSessions = R"({"success":true,"sessions":[
  {"client_ip":"192.168.1.20","aes_key":"00112233445566778899aabbccddeeff",
   "aes_iv":"01020304","rtsp_fake_ip":"10.1.2.3","video_width":1920,
   "video_height":1080,"video_refresh_rate":60,"audio_channel_count":2,
   "app_id":"1","client_id":"11111111111111111111"},
  {"client_ip":"192.168.1.20","aes_key":"ffeeddccbbaa99887766554433221100",
   "aes_iv":"05060708","rtsp_fake_ip":"10.4.5.6","video_width":1920,
   "video_height":1080,"video_refresh_rate":60,"audio_channel_count":2,
   "app_id":"1","client_id":"22222222222222222222"}]})";

/// GET /api/v1/lobbies. The first is what Wolf UI's plain "start" produces —
/// single-user, so unshareable; the second is what "start co-op" produces.
const char* const kLobbies = R"({"success":true,"lobbies":[
  {"id":"solo-lobby","name":"Firefox","multi_user":false,
   "started_by_profile_id":"p1","pin_required":false,
   "stop_when_everyone_leaves":true,"runner":{},
   "connected_sessions":["11111111111111111111"]},
  {"id":"coop-lobby","name":"Retro","multi_user":true,
   "started_by_profile_id":"p1","pin_required":false,
   "stop_when_everyone_leaves":true,"runner":{},
   "connected_sessions":["33333333333333333333"]}]})";

QJsonDocument doc(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json));
}

} // namespace

void run_wolf_coop_tests()
{
    SECTION("Wolf co-op — reading back the session our launch created");

    const QVector<WolfStreamSession> sessions = WolfApiClient::parseSessions(doc(kSessions));
    CHECK_EQ(sessions.size(), 2);
    // `client_id` is the field that carries the session id. There is no
    // `session_id` in this payload, and reading one would silently yield "".
    CHECK_EQ(sessions[0].sessionId, QStringLiteral("11111111111111111111"));
    CHECK_EQ(sessions[0].aesKey, QStringLiteral("00112233445566778899aabbccddeeff"));

    // The launch key is bytes on our side and lowercase hex on the wire.
    const QByteArray ourKey =
        QByteArray::fromHex("ffeeddccbbaa99887766554433221100");
    CHECK_EQ(WolfCoop::matchSessionByLaunchKey(sessions, ourKey),
             QStringLiteral("22222222222222222222"));

    // Case is not meaningful in hex; a host that upper-cased it must still match.
    QVector<WolfStreamSession> upper = sessions;
    upper[1].aesKey = upper[1].aesKey.toUpper();
    CHECK_EQ(WolfCoop::matchSessionByLaunchKey(upper, ourKey),
             QStringLiteral("22222222222222222222"));

    // No match is a legitimate answer (the session already ended), and so is an
    // empty key — neither may be turned into "some session, probably ours".
    CHECK(WolfCoop::matchSessionByLaunchKey(sessions, QByteArray::fromHex("0badc0de"))
              .isEmpty());
    CHECK(WolfCoop::matchSessionByLaunchKey(sessions, QByteArray()).isEmpty());
    CHECK(WolfCoop::matchSessionByLaunchKey({}, ourKey).isEmpty());

    SECTION("Wolf co-op — a lobby answer parses, multi_user flag and all");

    // The typed lobby mirror is transport we keep even though MoonlightWeb no
    // longer brokers the join itself (a guest joins natively from Wolf UI): the
    // wire shape is hard-won and worth pinning. The first lobby is what a plain
    // "start" produces — single-user; the second is a "start co-op" one.
    const QVector<WolfLobby> lobbies = WolfApiClient::parseLobbies(doc(kLobbies));
    CHECK_EQ(lobbies.size(), 2);
    CHECK(!lobbies[0].multiUser);
    CHECK(lobbies[1].multiUser);
    CHECK_EQ(lobbies[1].connectedSessions.size(), 1);
    CHECK_EQ(lobbies[1].id, QStringLiteral("coop-lobby"));
    CHECK_EQ(lobbies[1].connectedSessions.at(0), QStringLiteral("33333333333333333333"));

    SECTION("Wolf co-op — the PIN is digits, never the string a human typed");

    CHECK_EQ(WolfApiClient::pinDigits(QStringLiteral("1234")), (QVector<int>{1, 2, 3, 4}));
    CHECK_EQ(WolfApiClient::pinDigits(QStringLiteral("0000")), (QVector<int>{0, 0, 0, 0}));
    CHECK(WolfApiClient::pinDigits(QString()).isEmpty());
    // Anything non-numeric is not a PIN Wolf could hold, so it yields nothing
    // rather than a half-converted array that would fail the comparison anyway.
    CHECK(WolfApiClient::pinDigits(QStringLiteral("12a4")).isEmpty());

    SECTION("Wolf co-op — request bodies in the shape Wolf can parse");

    const QJsonObject join =
        WolfApiClient::joinLobbyBody(QStringLiteral("coop-lobby"),
                                     QStringLiteral("22222222222222222222"), {});
    CHECK_EQ(join.value(QStringLiteral("lobby_id")).toString(), QStringLiteral("coop-lobby"));
    // A STRING, although the field is an integer type upstream.
    CHECK(join.value(QStringLiteral("moonlight_session_id")).isString());
    CHECK_EQ(join.value(QStringLiteral("moonlight_session_id")).toString(),
             QStringLiteral("22222222222222222222"));
    // No PIN must mean the key is ABSENT, not an empty array: Wolf compares the
    // whole optional against the lobby's, so an empty array is "a PIN of no
    // digits" and fails to match a lobby that has none.
    CHECK(!join.contains(QStringLiteral("pin")));

    const QJsonObject joinPin = WolfApiClient::joinLobbyBody(
        QStringLiteral("coop-lobby"), QStringLiteral("2"), WolfApiClient::pinDigits("1234"));
    CHECK(joinPin.value(QStringLiteral("pin")).isArray());
    CHECK_EQ(joinPin.value(QStringLiteral("pin")).toArray().size(), 4);
    CHECK_EQ(joinPin.value(QStringLiteral("pin")).toArray().at(0).toInt(), 1);
    CHECK(!joinPin.value(QStringLiteral("pin")).isString());

    const QJsonObject leave =
        WolfApiClient::leaveLobbyBody(QStringLiteral("coop-lobby"), QStringLiteral("2"));
    CHECK_EQ(leave.value(QStringLiteral("lobby_id")).toString(), QStringLiteral("coop-lobby"));
    CHECK(leave.value(QStringLiteral("moonlight_session_id")).isString());
    CHECK(!leave.contains(QStringLiteral("pin")));

    const QJsonObject stopL = WolfApiClient::stopLobbyBody(QStringLiteral("coop-lobby"), {});
    CHECK_EQ(stopL.size(), 1);

    // The reaping call. Its field is `session_id` — a different name from the
    // `moonlight_session_id` the lobby calls use, for the same value.
    const QJsonObject stopS = WolfApiClient::stopSessionBody(QStringLiteral("22222222222222222222"));
    CHECK_EQ(stopS.size(), 1);
    CHECK(stopS.value(QStringLiteral("session_id")).isString());
    CHECK_EQ(stopS.value(QStringLiteral("session_id")).toString(),
             QStringLiteral("22222222222222222222"));

    SECTION("Wolf co-op — malformed answers yield nothing, never a wrong guess");

    CHECK(WolfApiClient::parseSessions(QJsonDocument()).isEmpty());
    CHECK(WolfApiClient::parseLobbies(QJsonDocument()).isEmpty());
    CHECK(WolfApiClient::parseSessions(doc(R"({"success":true})")).isEmpty());
    CHECK(WolfApiClient::parseLobbies(doc(R"({"success":true,"lobbies":[]})")).isEmpty());
    // A row with no id is dropped rather than kept as an unusable entry.
    CHECK(WolfApiClient::parseLobbies(doc(R"({"lobbies":[{"name":"x","multi_user":true}]})"))
              .isEmpty());
    CHECK(WolfApiClient::parseSessions(doc(R"({"sessions":[{"client_ip":"1.2.3.4"}]})")).isEmpty());
}
