/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/AppSettings.h"

#include <QTemporaryDir>
#include <QJsonObject>

void run_app_settings_tests()
{
    SECTION("AppSettings");

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    AppSettings s;
    // Redirect persistence to a hermetic temp file (m_FilePath is public).
    s.m_FilePath = tmp.path() + "/settings.json";
    CHECK_EQ(s.filePath(), s.m_FilePath);

    // Fallbacks when the file is empty / key absent.
    CHECK_EQ(s.httpPort(8080), quint16(8080));
    CHECK_EQ(s.httpsPort(8443), quint16(8443));

    // Round-trips: numbers and ports.
    s.setHttpPort(80);
    CHECK_EQ(s.httpPort(1), quint16(80));
    s.setHttpsPort(443);
    CHECK_EQ(s.httpsPort(1), quint16(443));
    s.setStreamBitrate(30000);
    CHECK_EQ(s.streamBitrate(), 30000);
    s.setStreamHeight(1440);
    CHECK_EQ(s.streamHeight(), 1440);
    s.setStreamFps(120);
    CHECK_EQ(s.streamFps(), 120);

    // Video codec enum <-> string mapping.
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::H264), QString("h264"));
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::HEVC), QString("hevc"));
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::AV1), QString("av1"));
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::Auto), QString("auto"));
    CHECK(AppSettings::videoCodecFromString("av1") == VideoCodec::AV1);
    CHECK(AppSettings::videoCodecFromString("garbage") == VideoCodec::Auto);
    s.setVideoCodec(VideoCodec::HEVC);
    CHECK(s.videoCodec() == VideoCodec::HEVC);

    // Boolean round-trips.
    s.setGamingMode(true);
    CHECK_EQ(s.gamingMode(), true);
    s.setShowPerformanceStats(true);
    CHECK_EQ(s.showPerformanceStats(), true);
    s.setHdrEnabled(true);
    CHECK_EQ(s.hdrEnabled(), true);
    s.setChroma444Enabled(true);
    CHECK_EQ(s.chroma444Enabled(), true);
    s.setUpnpEnabled(false);
    CHECK_EQ(s.upnpEnabled(), false);
    s.setAutoIpDetection(false);
    CHECK_EQ(s.autoIpDetection(), false);
    s.setInternetAccessEnabled(true);
    CHECK_EQ(s.internetAccessEnabled(), true);
    s.setPendingRegistration(true);
    CHECK_EQ(s.pendingRegistration(), true);
    s.setCertAuthEnabled(true);
    CHECK_EQ(s.certAuthEnabled(), true);

    // String round-trips.
    s.setStreamAspect("21:9");
    CHECK_EQ(s.streamAspect(), QString("21:9"));
    s.setVideoEnhancement("on");
    CHECK_EQ(s.videoEnhancement(), QString("on"));
    s.setVideoEnhancementAlgo("fsr1");
    CHECK_EQ(s.videoEnhancementAlgo(), QString("fsr1"));
    s.setStunServer("stun:example.org:3478");
    CHECK_EQ(s.stunServer(), QString("stun:example.org:3478"));
    s.setTransport("wss");
    CHECK_EQ(s.transport(), QString("wss"));
    s.setTransportMode("webrtc-dc-udp");
    CHECK_EQ(s.transportMode(), QString("webrtc-dc-udp"));
    s.setUniqueId("abcd1234");
    CHECK_EQ(s.uniqueId(), QString("abcd1234"));
    s.setRegisteredUid("abcd1234");
    CHECK_EQ(s.registeredUid(), QString("abcd1234"));
    s.setPublicIp("1.2.3.4");
    CHECK_EQ(s.publicIp(), QString("1.2.3.4"));
    s.setCertPem("MW_CERT_PEM");
    CHECK_EQ(s.certPem(), QString("MW_CERT_PEM"));
    s.setCertKey("MW_CERT_KEY");
    CHECK_EQ(s.certKey(), QString("MW_CERT_KEY"));
    s.setCertificateToken("token-xyz");
    CHECK_EQ(s.certificateToken(), QString("token-xyz"));
    s.setOwnerToken("owner-xyz");
    CHECK_EQ(s.ownerToken(), QString("owner-xyz"));

    // HMAC key (Base64 binary) round-trip.
    QByteArray key("\x01\x02\x03\x04binarykey", 13);
    s.setHmacKey(key);
    CHECK_EQ(s.hmacKey(), key);

    // domain(): a stored FQDN is returned as-is; otherwise built from uniqueId.
    s.setDomain("my.host.example.com");
    CHECK_EQ(s.domain(), QString("my.host.example.com"));

    // Documented file-only defaults are idempotently seeded.
    s.seedDocumentedDefaults();
    s.audioTimeStretch(); // exercised (value documented as true by default)

    // Low-level access.
    QJsonObject all = s.readAll();
    CHECK(all.contains("http_port"));
    all["custom_key"] = 99;
    s.writeAll(all);
    CHECK_EQ(s.readAll().value("custom_key").toInt(), 99);

    // FQDN validation.
    CHECK(AppSettings::isValidFqdn("host.example.com"));
    CHECK(AppSettings::isValidFqdn("a.b"));
    CHECK(!AppSettings::isValidFqdn("nodot"));
    CHECK(!AppSettings::isValidFqdn(".leadingdot.com"));
    CHECK(!AppSettings::isValidFqdn("trailingdot."));
    CHECK(!AppSettings::isValidFqdn(""));
}
