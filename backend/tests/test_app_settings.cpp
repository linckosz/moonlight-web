/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/AppSettings.h"

#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
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
    // A LAN-only instance reads it as off, whatever the file says.
    qputenv("MW_LAN_ONLY", "1");
    CHECK_EQ(s.internetAccessEnabled(), false);
    qunsetenv("MW_LAN_ONLY");
    CHECK_EQ(s.internetAccessEnabled(), true);
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
    s.setTransportMode("webrtc-dc-udp");
    CHECK_EQ(s.transportMode(), QString("webrtc-dc-udp"));
    s.setUniqueId("abcd1234");
    CHECK_EQ(s.uniqueId(), QString("abcd1234"));

    // The desktop portal's consent. Empty until one is granted — that emptiness
    // is what makes the very first session raise a dialog, so it is the answer
    // this getter has to give on a fresh install, not a placeholder.
    CHECK_EQ(s.portalRestoreToken(), QString());
    s.setPortalRestoreToken("portal-token-1");
    CHECK_EQ(s.portalRestoreToken(), QString("portal-token-1"));
    // Stored verbatim, never parsed: the token means something to the portal
    // that issued it and to nothing else, so anything it hands back survives
    // the round trip unchanged.
    s.setPortalRestoreToken("a/b+c=~ \xC3\xA9");
    CHECK_EQ(s.portalRestoreToken(), QString::fromUtf8("a/b+c=~ \xC3\xA9"));
    // Re-granted: a portal that asked again wins over what was remembered.
    s.setPortalRestoreToken("portal-token-2");
    CHECK_EQ(s.portalRestoreToken(), QString("portal-token-2"));
    // Writing the same value again is a no-op, not a rewrite of settings.json:
    // every session on the portal route would otherwise touch the file for a
    // token that did not change. Proved by re-writing the very same object by
    // hand in COMPACT form — writeAll() always indents, so a file that gained
    // newlines is a file that was written.
    {
        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject same = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(same).toJson(QJsonDocument::Compact));
        f.close();

        s.setPortalRestoreToken("portal-token-2");
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(!f.readAll().contains('\n'));
        f.close();
        CHECK_EQ(s.portalRestoreToken(), QString("portal-token-2"));

        // The positive control, without which the check above passes for a
        // getter that does nothing at all: a DIFFERENT token does write.
        s.setPortalRestoreToken("portal-token-3");
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll().contains('\n'));
        f.close();
        CHECK_EQ(s.portalRestoreToken(), QString("portal-token-3"));
    }

    s.setPublicIp("1.2.3.4");
    CHECK_EQ(s.publicIp(), QString("1.2.3.4"));
    s.setCertPem("MW_CERT_PEM");
    CHECK_EQ(s.certPem(), QString("MW_CERT_PEM"));
    s.setCertKey("MW_CERT_KEY");
    CHECK_EQ(s.certKey(), QString("MW_CERT_KEY"));
    s.setCertificateToken("token-xyz");
    CHECK_EQ(s.certificateToken(), QString("token-xyz"));

    // HMAC key (Base64 binary) round-trip.
    QByteArray key("\x01\x02\x03\x04binarykey", 13);
    s.setHmacKey(key);
    CHECK_EQ(s.hmacKey(), key);

    // domain(): a stored FQDN is a user-owned domain and must survive verbatim.
    // Everything else resolves to nothing — this host registers no name, and
    // handing out a sub-domain nothing points at would poison the Host-header
    // trust and every entry-point URL.
    qputenv("MW_DOMAIN", "example.test");
    s.setUniqueId("abcd1234");
    s.setDomain("MW_DOMAIN");
    CHECK_EQ(s.domain(), QString());
    s.setDomain("my.host.example.com");
    CHECK_EQ(s.domain(), QString("my.host.example.com"));
    // A stored value that is not a valid FQDN is not a domain at all.
    s.setDomain("nodot");
    CHECK_EQ(s.domain(), QString());
    // The name a pre-0.3 install registered is excluded by name: it is left in
    // settings.json on every upgraded machine, and it is not the user's.
    s.setDomain("abcd1234.example.test");
    CHECK_EQ(s.domain(), QString());
    s.setDomain("MW_DOMAIN");
    qunsetenv("MW_DOMAIN");

    // Consent record: versioned since the DNS mechanism started retiring. A
    // fresh write carries version 2 + the mechanism it was worded for; the
    // version reader maps "no record" to 0 (a legacy record without the field
    // reads as 1, the DNS-era wording).
    CHECK_EQ(s.internetConsentVersion(), 0);
    s.setInternetConsent("agreement text", "admin", "rendezvous");
    CHECK_EQ(s.internetConsentVersion(), 2);
    CHECK_EQ(s.internetConsent().value("mechanism").toString(), QString("rendezvous"));
    CHECK_EQ(s.internetConsent().value("source").toString(), QString("admin"));

    // ── Statistics consent ────────────────────────────────────────────────
    // Silence is not consent: until the question has been answered, both
    // censuses must stay shut whatever the file-only switches say.
    CHECK_EQ(s.metricsConsentDecision(), QString());
    CHECK(!s.sessionMetricsAllowed());
    CHECK(!s.updateRelayAllowed());

    s.setMetricsConsent(false, "the wording that was displayed", "banner");
    CHECK_EQ(s.metricsConsentDecision(), QString("denied"));
    CHECK(!s.sessionMetricsAllowed());
    CHECK(!s.updateRelayAllowed());

    s.setMetricsConsent(true, "the wording that was displayed", "banner");
    CHECK_EQ(s.metricsConsentDecision(), QString("granted"));
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());
    // The record has to say what was agreed to, and when.
    CHECK_EQ(s.metricsConsent().value("message").toString(),
             QString("the wording that was displayed"));
    CHECK_EQ(s.metricsConsent().value("source").toString(), QString("banner"));
    CHECK(!s.metricsConsent().value("at").toString().isEmpty());

    // A file-only opt-out still wins over a granted consent.
    {
        QJsonObject obj = s.readAll();
        obj["session_metrics_enabled"] = false;
        s.writeAll(obj);
        CHECK(!s.sessionMetricsAllowed());
        CHECK(s.updateRelayAllowed()); // the other switch is untouched
        obj = s.readAll();
        obj["session_metrics_enabled"] = true;
        s.writeAll(obj);
    }

    // An answer given to an older, narrower wording does not carry over to a
    // question that describes more: it reads as unasked, and reports nothing.
    {
        QJsonObject obj = s.readAll();
        QJsonObject consent = obj["metrics_consent"].toObject();
        consent["version"] = AppSettings::kMetricsConsentVersion - 1;
        obj["metrics_consent"] = consent;
        s.writeAll(obj);
        CHECK_EQ(s.metricsConsentDecision(), QString());
        CHECK(!s.sessionMetricsAllowed());
        CHECK(!s.updateRelayAllowed());
    }

    // Documented file-only defaults are idempotently seeded.
    s.seedDocumentedDefaults();
    s.audioTimeStretch(); // exercised (value documented as true by default)
    CHECK(s.readAll().contains("native_host_enabled"));

    // The native host is offered unless someone says otherwise — and an absent
    // key says nothing, which is the state every install before this setting is
    // in. Only an explicit false hides the card.
    {
        QJsonObject obj = s.readAll();
        obj.remove("native_host_enabled");
        s.writeAll(obj);
        CHECK(s.nativeHostEnabled());

        s.setNativeHostEnabled(false);
        CHECK(!s.nativeHostEnabled());
        s.setNativeHostEnabled(true);
        CHECK(s.nativeHostEnabled());
    }

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
