/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A stream the server counted on its shared ports that ends up on a socket of
 * its own (streaming/MuxRefuse.h): the worker's word read strictly, the
 * server's answer read strictly, no claim while quitting or for a superseded
 * launch, an answer taken off at once while the server quits, and otherwise
 * kept or let go as any hole of the slot is (an owner's kept, a guest's on the
 * guests' grace).
 */
#include "test_framework.h"
#include "streaming/MuxRefuse.h"

#include <QJsonArray>

using namespace mw::muxrefuse;

namespace {

void theWord()
{
    const auto refused = QJsonObject{{"event", "iceMux"}, {"op", "refused"}};
    CHECK(isRefusedEvent(refused));
    // Anything more, or anything else, is not that word
    QJsonObject more = refused;
    more["port"] = 48550;
    CHECK(!isRefusedEvent(more));
    more = refused;
    more["why"] = "wrong sockets";
    CHECK(!isRefusedEvent(more));
    CHECK(!isRefusedEvent(QJsonObject{{"event", "iceMux"}, {"op", "refused "}}));
    CHECK(!isRefusedEvent(QJsonObject{{"event", "iceMux"}, {"op", "Refused"}}));
    CHECK(!isRefusedEvent(QJsonObject{{"event", "iceMux"}, {"op", true}}));
    CHECK(!isRefusedEvent(QJsonObject{{"event", "icemux"}, {"op", "refused"}}));
    CHECK(!isRefusedEvent(QJsonObject{{"event", "iceMux"}, {"op", "agent"}}));
    CHECK(!isRefusedEvent(QJsonObject{{"op", "refused"}}));
    CHECK(!isRefusedEvent(QJsonObject{}));
}

void theAnswer()
{
    QString ip = QStringLiteral("unchanged");
    quint16 port = 7;
    // A hole: an address and a port, and nothing else
    CHECK(parseHoleCommand(QJsonObject{{"cmd", "iceMuxHole"},
                                       {"upnpPublicIp", "203.0.113.7"},
                                       {"upnpExternalPort", 46100}},
                           &ip, &port));
    CHECK_EQ(ip, QStringLiteral("203.0.113.7"));
    CHECK_EQ(port, quint16(46100));
    CHECK(parseHoleCommand(QJsonObject{{"cmd", "iceMuxHole"},
                                       {"upnpPublicIp", "2001:db8::7"},
                                       {"upnpExternalPort", 65535}},
                           &ip, &port));
    CHECK_EQ(port, quint16(65535));
    // No hole: port 0 and no address
    CHECK(parseHoleCommand(
        QJsonObject{{"cmd", "iceMuxHole"}, {"upnpPublicIp", ""}, {"upnpExternalPort", 0}}, &ip,
        &port));
    CHECK(ip.isEmpty());
    CHECK_EQ(port, quint16(0));

    // Refused, and nothing changed
    ip = QStringLiteral("unchanged");
    port = 7;
    const auto refuses = [&ip, &port](const QJsonObject& cmd) {
        return !parseHoleCommand(cmd, &ip, &port) && ip == QStringLiteral("unchanged") && port == 7;
    };
    CHECK(refuses(QJsonObject{{"cmd", "iceMuxHole"}, {"upnpPublicIp", "203.0.113.7"}}));
    CHECK(refuses(QJsonObject{{"cmd", "iceMuxHole"},
                              {"upnpPublicIp", "203.0.113.7"},
                              {"upnpExternalPort", 46100},
                              {"internalPort", 48550}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxhole"}, {"upnpPublicIp", "203.0.113.7"}, {"upnpExternalPort", 46100}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxHole"}, {"upnpPublicIp", "203.0.113.7"}, {"upnpExternalPort", 65536}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxHole"}, {"upnpPublicIp", "203.0.113.7"}, {"upnpExternalPort", -1}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxHole"}, {"upnpPublicIp", "203.0.113.7"}, {"upnpExternalPort", 46100.5}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxHole"}, {"upnpPublicIp", "203.0.113.7"}, {"upnpExternalPort", "46100"}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxHole"}, {"upnpPublicIp", "not an address"}, {"upnpExternalPort", 46100}}));
    CHECK(refuses(
        QJsonObject{{"cmd", "iceMuxHole"}, {"upnpPublicIp", ""}, {"upnpExternalPort", 46100}}));
    CHECK(refuses(QJsonObject{
        {"cmd", "iceMuxHole"}, {"upnpPublicIp", "203.0.113.7"}, {"upnpExternalPort", 0}}));
    CHECK(refuses(
        QJsonObject{{"cmd", "iceMuxHole"}, {"upnpPublicIp", 7}, {"upnpExternalPort", 46100}}));
    CHECK(refuses(QJsonObject{{"cmd", "iceMuxHole"},
                              {"upnpPublicIp", QJsonArray{"203.0.113.7"}},
                              {"upnpExternalPort", 46100}}));
}

void whenToAsk()
{
    // Quitting (Security Review M1): nothing, whatever else
    CHECK(!claimWhenAsked(true, true));
    CHECK(!claimWhenAsked(true, false));
    // A newer launch on the slot
    CHECK(!claimWhenAsked(false, false));
    CHECK(claimWhenAsked(false, true));
}

void whatToDoWithTheAnswer()
{
    // The stream is there and current: it is told, owner's slot or guest's
    CHECK(onAnswer(false, true, true, true, false) == Answer::Use);
    CHECK(onAnswer(false, true, true, true, true) == Answer::Use);
    // No hole came: nothing to keep or take off
    CHECK(onAnswer(false, true, true, false, false) == Answer::Nothing);
    CHECK(onAnswer(true, false, false, false, true) == Answer::Nothing);
    // The server quits: taken off at once, whatever the slot (M1)
    CHECK(onAnswer(true, true, true, true, false) == Answer::Release);
    CHECK(onAnswer(true, false, false, true, true) == Answer::Release);
    // A launch that is over (superseded, or its worker gone): an owner's slot
    // keeps it as its held hole, as any hole claimed before a launch…
    CHECK(onAnswer(false, false, true, true, false) == Answer::Nothing);
    CHECK(onAnswer(false, true, false, true, false) == Answer::Nothing);
    // …and a guest's goes the way a guest's hole goes
    CHECK(onAnswer(false, false, true, true, true) == Answer::Guest);
    CHECK(onAnswer(false, true, false, true, true) == Answer::Guest);
}

} // namespace

void run_mux_refuse_tests()
{
    theWord();
    theAnswer();
    whenToAsk();
    whatToDoWithTheAnswer();
}
