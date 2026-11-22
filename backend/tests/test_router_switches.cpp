// The router's switches in their two routes (server/RouterSwitchesWrite.h):
// POST /api/admin/settings writes the two boxes and nothing else of the three,
// POST /api/settings/streaming writes "upnp_enabled" and refuses a box; in
// each, a value that is not a JSON boolean refuses the whole body before
// anything is written, and a write touches nothing it does not name.
#include "test_framework.h"
#include "server/AppSettings.h"
#include "server/RouterSwitchesWrite.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

void run_router_switches_tests()
{
    namespace rs = mw::routerswitches;
    const QJsonValue bads[] = {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null),
                               QJsonValue(QJsonObject{}), QJsonValue(QJsonArray{})};

    SECTION("RouterSwitches: the admin route refuses a box that is not a boolean");
    {
        CHECK(rs::adminRefused({}).isEmpty());
        CHECK(rs::adminRefused({{"ice_udp_mux", false}, {"ice_tcp_443", true}}).isEmpty());
        CHECK(rs::adminRefused({{"cert_auth_enabled", "anything"}}).isEmpty()); // not ours
        // UPnP is not the admin route's: it is neither checked nor written there.
        CHECK(rs::adminRefused({{"upnp_enabled", "anything"}}).isEmpty());
        for (const char* key : {"ice_udp_mux", "ice_tcp_443"}) {
            for (const QJsonValue& bad : bads) {
                QJsonObject body{{"ice_udp_mux", false}, {"ice_tcp_443", false}};
                body[QLatin1String(key)] = bad;
                CHECK(rs::adminRefused(body).contains(QLatin1String(key)));
            }
        }
    }

    SECTION("RouterSwitches: the streaming route refuses a box, and a UPnP that is not a boolean");
    {
        CHECK(rs::streamingRefused({}).isEmpty());
        CHECK(rs::streamingRefused({{"upnp_enabled", true}}).isEmpty());
        CHECK(rs::streamingRefused({{"transport_mode", "auto"}}).isEmpty());
        for (const QJsonValue& bad : bads)
            CHECK(rs::streamingRefused({{"upnp_enabled", bad}}).contains("upnp_enabled"));
        for (const char* key : {"ice_udp_mux", "ice_tcp_443"}) {
            for (const QJsonValue v : {QJsonValue(true), QJsonValue(false), QJsonValue("no")}) {
                const QString why =
                    rs::streamingRefused({{"upnp_enabled", true}, {QLatin1String(key), v}});
                CHECK(why.contains(QLatin1String(key)));
                CHECK(why.contains("admin"));
            }
        }
    }

    SECTION("RouterSwitches: the admin route writes the two boxes, in one go, and nothing else");
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        AppSettings::setFileOverride(tmp.path() + "/settings.json");
        AppSettings s;
        const QJsonObject before{{"upnp_enabled", true}, {"cert_auth_enabled", true}};
        s.writeAll(before);

        QJsonObject reply;
        CHECK(!rs::adminWrite({{"cert_auth_enabled", false}}, s, reply).any());
        CHECK(!rs::adminWrite({{"upnp_enabled", false}}, s, reply).any());
        CHECK(reply.isEmpty());
        CHECK(s.readAll() == before);

        // What the admin page sends when the box of the shared ports is unticked.
        rs::BoxesWritten done = rs::adminWrite({{"ice_udp_mux", false}}, s, reply);
        CHECK(done.sharedPorts);
        CHECK(!done.tcp443);
        CHECK(reply.value("ice_udp_mux") == QJsonValue(false));
        CHECK(reply.value("status") == QJsonValue("saved"));
        CHECK(s.readAll().value("ice_udp_mux") == QJsonValue(false));
        CHECK(!s.iceUdpMux());

        reply = {};
        done = rs::adminWrite({{"ice_udp_mux", true},
                               {"ice_tcp_443", false},
                               {"upnp_enabled", false},
                               {"debug_dev_standard_ports", true},
                               {"debug_ice_mux_refuse", true}},
                              s, reply);
        CHECK(done.sharedPorts);
        CHECK(done.tcp443);
        CHECK(reply.value("ice_udp_mux") == QJsonValue(AppSettings::sharedPortsByDefault()));
        CHECK(reply.value("ice_tcp_443") == QJsonValue(false));
        CHECK(!reply.contains("upnp_enabled"));
        const QJsonObject after = s.readAll();
        CHECK(after.value("upnp_enabled") == QJsonValue(true)); // not the admin route's
        CHECK(after.value("cert_auth_enabled") == QJsonValue(true));
        CHECK_EQ(after.contains("ice_udp_mux"), !AppSettings::sharedPortsByDefault());
        CHECK(after.value("ice_tcp_443") == QJsonValue(false));
        CHECK(!after.contains("debug_dev_standard_ports"));
        CHECK(!after.contains("debug_ice_mux_refuse"));

        AppSettings::setFileOverride(QString());
    }

    SECTION("RouterSwitches: the streaming route writes UPnP alone");
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        AppSettings::setFileOverride(tmp.path() + "/settings.json");
        AppSettings s;
        const QJsonObject before{{"upnp_enabled", true}, {"ice_tcp_443", true}};
        s.writeAll(before);

        QJsonObject reply;
        CHECK(!rs::streamingWrite({{"transport_mode", "auto"}}, s, reply));
        CHECK(reply.isEmpty());
        CHECK(s.readAll() == before);

        CHECK(rs::streamingWrite({{"upnp_enabled", false},
                                  {"debug_dev_standard_ports", true},
                                  {"debug_ice_mux_refuse", true}},
                                 s, reply));
        CHECK(reply.value("upnp_enabled") == QJsonValue(false));
        CHECK(!s.upnpEnabled());
        const QJsonObject after = s.readAll();
        CHECK(after.value("ice_tcp_443") == QJsonValue(true));
        CHECK(!after.contains("ice_udp_mux"));
        CHECK(!after.contains("debug_dev_standard_ports"));
        CHECK(!after.contains("debug_ice_mux_refuse"));

        AppSettings::setFileOverride(QString());
    }
}
