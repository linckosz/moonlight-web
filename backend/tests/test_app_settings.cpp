/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/AppSettings.h"
#include "common/Logger.h"

#include "mw/native/VideoPipeline.h"

#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// Holds a file so that nobody else can read it, for as long as it lives, while
// it can still be replaced by a rename: a handle shared for delete only on
// Windows, no permissions elsewhere. held() is false where that does not stop
// this process (running as root).
class FileHold
{
public:
    explicit FileHold(const QString& path)
        : m_Path(path)
    {
#ifdef Q_OS_WIN
        m_Handle = CreateFileW(
            reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ,
            FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        m_Perms = QFile::permissions(path);
        QFile::setPermissions(path, QFileDevice::Permissions{});
#endif
        QFile probe(path);
        m_Held = !probe.open(QIODevice::ReadOnly);
    }
    ~FileHold()
    {
#ifdef Q_OS_WIN
        if (m_Handle != INVALID_HANDLE_VALUE) CloseHandle(m_Handle);
#else
        QFile::setPermissions(m_Path, m_Perms);
#endif
    }
    bool held() const { return m_Held; }

private:
    QString m_Path;
    bool m_Held = false;
#ifdef Q_OS_WIN
    HANDLE m_Handle = INVALID_HANDLE_VALUE;
#else
    QFileDevice::Permissions m_Perms;
#endif
};
} // namespace

void run_app_settings_tests()
{
    SECTION("AppSettings: audio_source reads in its one form, all apps otherwise");
    CHECK_EQ(AppSettings::canonicalAudioSource("focused"), QString("focused"));
    CHECK_EQ(AppSettings::canonicalAudioSource("app:Discord.EXE"), QString("app:discord.exe"));
    for (const char* other :
         {"", "all", "Focused", "app:", "app:discord", "app:a\\b.exe", "app:a/b.exe"})
        CHECK_EQ(AppSettings::canonicalAudioSource(other), QString("all"));

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
    CHECK_EQ(s.packetMarking(), true); // marked unless the admin unticks it
    s.setPacketMarking(false);
    CHECK_EQ(s.packetMarking(), false);
    CHECK_EQ(s.iceTcpPassive(), true);    // listens while the checks run, since 0.4.1
    CHECK_EQ(s.debugIceDropUdp(), false); // a bench switch, never on by itself
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

    // The picture chain of a native session (admin → Advanced): auto until
    // the admin picks one — Windows' names and Linux's, in any case, nothing
    // else. A refused value leaves the stored one in place, and a file edited
    // by hand to something else reads as the default, not as a chain that
    // does not exist.
    CHECK_EQ(s.nativeVideoPipeline(), QString("auto"));
    CHECK(s.setNativeVideoPipeline("Vulkan"));
    CHECK_EQ(s.nativeVideoPipeline(), QString("vulkan"));
    CHECK(s.setNativeVideoPipeline("vaapi"));
    CHECK_EQ(s.nativeVideoPipeline(), QString("vaapi"));
    CHECK(s.setNativeVideoPipeline("d3d12"));
    CHECK_EQ(s.nativeVideoPipeline(), QString("d3d12"));
    CHECK(s.setNativeVideoPipeline(" D3D11 "));
    CHECK_EQ(s.nativeVideoPipeline(), QString("d3d11"));
    CHECK(!s.setNativeVideoPipeline("d3d13"));
    CHECK(!s.setNativeVideoPipeline(""));
    CHECK_EQ(s.nativeVideoPipeline(), QString("d3d11"));
    {
        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject edited = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        edited["native_video_pipeline"] = "d3d10";
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(edited).toJson());
        f.close();
        CHECK_EQ(s.nativeVideoPipeline(), QString("auto"));
    }
    // What the worker is handed is what the engine reads: every name the
    // setting can answer parses on the native side, to the chain it names.
    {
        const struct
        {
            const char* stored;
            mw::native::VideoPipeline engine;
        } names[] = {{"auto", mw::native::VideoPipeline::Auto},
                     {"d3d11", mw::native::VideoPipeline::D3d11},
                     {"d3d12", mw::native::VideoPipeline::D3d12},
                     {"vaapi", mw::native::VideoPipeline::Vaapi},
                     {"vulkan", mw::native::VideoPipeline::Vulkan}};
        for (const auto& n : names) {
            CHECK(s.setNativeVideoPipeline(n.stored));
            mw::native::VideoPipeline parsed = mw::native::VideoPipeline::Auto;
            CHECK(mw::native::parseVideoPipeline(s.nativeVideoPipeline().toStdString(), parsed));
            CHECK(parsed == n.engine);
        }
    }
    // The native host's apps: none until some are set; stored trimmed with
    // server-made ids, refused whole when one entry is not valid — the file is
    // then left as it was — and an entry edited by hand into nonsense left out.
    {
        CHECK_EQ(s.hostApps().size(), 0);
        QJsonArray apps;
        apps.append(QJsonObject{{"name", "  RetroArch "}, {"command", " retroarch -f "}});
        apps.append(QJsonObject{{"name", "Heroic"}, {"command", "heroic --no-gui"}});
        QString why;
        CHECK(s.setHostApps(apps, &why));
        QJsonArray stored = s.hostApps();
        CHECK_EQ(stored.size(), 2);
        const QJsonObject retro = stored.at(0).toObject();
        CHECK_EQ(retro.value("name").toString(), QString("RetroArch"));
        CHECK_EQ(retro.value("command").toString(), QString("retroarch -f"));
        CHECK_EQ(retro.value("source").toString(), QString("user"));
        CHECK_EQ(retro.value("app_id").toInt(), AppSettings::kHostAppIdFirst);
        CHECK_EQ(retro.value("id").toString().size(), 36);
        CHECK_EQ(stored.at(1).toObject().value("app_id").toInt(), AppSettings::kHostAppIdFirst + 1);
        CHECK_EQ(s.hostApp(AppSettings::kHostAppIdFirst + 1).value("name").toString(),
                 QString("Heroic"));
        CHECK(s.hostApp(4242).isEmpty());

        QJsonArray bad = apps;
        bad.append(QJsonObject{{"name", "Two lines"}, {"command", "a\nb"}});
        CHECK(!s.setHostApps(bad, &why));
        CHECK(why.contains("one line"));
        QJsonArray unnamed;
        unnamed.append(QJsonObject{{"name", "   "}, {"command", "x"}});
        CHECK(!s.setHostApps(unnamed, &why));
        QJsonArray tooLong;
        tooLong.append(QJsonObject{{"name", QString(41, 'n')}, {"command", "x"}});
        CHECK(!s.setHostApps(tooLong, &why));
        QJsonArray tooMany;
        for (int i = 0; i <= AppSettings::kHostAppsMax; ++i)
            tooMany.append(QJsonObject{{"name", QString("App %1").arg(i)}, {"command", "x"}});
        CHECK(!s.setHostApps(tooMany, &why));
        CHECK_EQ(s.hostApps().size(), 2);

        // Removing the first keeps the second's card id, and a new one gets a
        // fresh id — never the removed one's. The page cannot set the server's
        // fields: a forged app_id, source or image is ignored.
        QJsonObject heroic = s.hostApps().at(1).toObject();
        heroic["app_id"] = 1;
        heroic["source"] = "steam";
        heroic["image"] = "0123456789abcdef";
        QJsonArray edited;
        edited.append(heroic);
        edited.append(QJsonObject{{"name", "New"}, {"command", "new"}, {"app_id", 5}});
        QStringList removed;
        CHECK(s.setHostApps(edited, &why, &removed));
        CHECK_EQ(removed.size(), 1);
        CHECK_EQ(removed.at(0), retro.value("id").toString());
        stored = s.hostApps();
        CHECK_EQ(stored.at(0).toObject().value("app_id").toInt(), AppSettings::kHostAppIdFirst + 1);
        CHECK_EQ(stored.at(0).toObject().value("source").toString(), QString("user"));
        CHECK(!stored.at(0).toObject().contains("image"));
        CHECK_EQ(stored.at(1).toObject().value("app_id").toInt(), AppSettings::kHostAppIdFirst + 2);

        // The image's hash is the server's to record.
        const QString heroicId = stored.at(0).toObject().value("id").toString();
        CHECK(s.setHostAppImage(heroicId, "0123456789abcdef"));
        CHECK_EQ(s.hostApps().at(0).toObject().value("image").toString(),
                 QString("0123456789abcdef"));
        CHECK(s.setHostAppImage(heroicId, QString()));
        CHECK(!s.hostApps().at(0).toObject().contains("image"));
        CHECK(!s.setHostAppImage("not-an-id", "0123456789abcdef"));

        // Hand-edited nonsense is left out.
        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject file = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonArray hand = file["host_apps"].toArray();
        hand.append(QJsonObject{{"name", "Hand-made"}});
        hand.append(QString("not an object"));
        file["host_apps"] = hand;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(file).toJson());
        f.close();
        CHECK_EQ(s.hostApps().size(), 2);

        CHECK(s.setHostApps(QJsonArray(), &why));
        CHECK_EQ(s.hostApps().size(), 0);
    }
    // Launchers found on this PC: seeded once per version; a seeded card the
    // owner removes is dismissed, never seeded again, and can be restored.
    {
        QJsonArray found;
        found.append(QJsonObject{{"source", "steam"},
                                 {"name", "Steam Big Picture"},
                                 {"command", "steam://open/bigpicture"},
                                 {"quit_command", "steam://close/bigpicture"}});
        found.append(QJsonObject{{"source", "user"}, {"name", "Not a launcher"}, {"command", "x"}});
        QJsonArray added;
        CHECK(s.seedHostApps(found, "0.4.0", &added));
        CHECK_EQ(added.size(), 1);
        CHECK_EQ(s.hostApps().size(), 1);
        const QJsonObject steam = s.hostApps().at(0).toObject();
        CHECK_EQ(steam.value("source").toString(), QString("steam"));
        CHECK_EQ(steam.value("quit_command").toString(), QString("steam://close/bigpicture"));
        // Same version: nothing more, even a launcher found since.
        CHECK(!s.seedHostApps(found, "0.4.0", &added));

        // Renamed by the owner, it stays the Steam card.
        QJsonArray renamed;
        QJsonObject mine = steam;
        mine["name"] = "Big Picture";
        renamed.append(mine);
        QString why;
        CHECK(s.setHostApps(renamed, &why));
        CHECK_EQ(s.hostApps().at(0).toObject().value("source").toString(), QString("steam"));
        CHECK_EQ(s.hostApps().at(0).toObject().value("quit_command").toString(),
                 QString("steam://close/bigpicture"));

        // Removed: dismissed, and the next version leaves it out.
        CHECK(s.setHostApps(QJsonArray(), &why));
        CHECK_EQ(s.hostAppsDismissed(), QStringList{"steam"});
        added = QJsonArray();
        CHECK(s.seedHostApps(found, "0.4.1", &added));
        CHECK_EQ(added.size(), 0);
        CHECK_EQ(s.hostApps().size(), 0);

        // Restored at the owner's request: back, with a new card id.
        const QJsonObject back = s.restoreHostApp(found.at(0).toObject(), &why);
        CHECK(!back.isEmpty());
        CHECK(back.value("app_id").toInt() > steam.value("app_id").toInt());
        CHECK(s.hostAppsDismissed().isEmpty());
        CHECK(s.restoreHostApp(found.at(0).toObject(), &why).isEmpty());
        CHECK(why.contains("already"));
        CHECK(s.setHostApps(QJsonArray(), &why));
    }
    // The betas' gamescope_apps move to host_apps, keeping their card ids.
    {
        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject file = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        file.remove("host_apps");
        QJsonArray old;
        old.append(QJsonObject{{"name", "RetroArch"}, {"command", "retroarch"}});
        old.append(QJsonObject{{"name", "Lutris"}, {"command", "lutris"}});
        file["gamescope_apps"] = old;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(file).toJson());
        f.close();
        s.migrateHostApps();
        const QJsonArray moved = s.hostApps();
        CHECK_EQ(moved.size(), 2);
        CHECK_EQ(moved.at(0).toObject().value("app_id").toInt(), 1100);
        CHECK_EQ(moved.at(1).toObject().value("app_id").toInt(), 1101);
        CHECK_EQ(moved.at(1).toObject().value("name").toString(), QString("Lutris"));
        CHECK(!s.readAll().contains("gamescope_apps"));
        s.migrateHostApps(); // idempotent
        CHECK_EQ(s.hostApps().size(), 2);
        QString why;
        CHECK(s.setHostApps(QJsonArray(), &why));
    }

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

    // The display rates the capture did not hold (vddmatch): none on a fresh
    // install; one per GPU and rate; the same refusal again counts one more
    // in a row and is dated anew; a retry that holds clears it; another app
    // version's entries go with the next refusal.
    CHECK_EQ(s.vddMatchRefused().size(), 0);
    CHECK_EQ(s.addVddMatchRefusal("GPU A", 330000, "1.0", 1000), 1);
    s.addVddMatchRefusal("GPU A", 288000, "1.0", 1000);
    s.addVddMatchRefusal("GPU B", 330000, "1.0", 1000);
    CHECK_EQ(s.addVddMatchRefusal("GPU A", 330000, "1.0", 5000), 2);
    CHECK_EQ(s.vddMatchRefused().size(), 3);
    {
        const QJsonObject again = s.vddMatchRefused().at(2).toObject();
        CHECK_EQ(again.value("gpu").toString(), QString("GPU A"));
        CHECK_EQ(again.value("milli_hz").toInt(), 330000);
        CHECK_EQ(again.value("version").toString(), QString("1.0"));
        CHECK_EQ(again.value("at").toInteger(), 5000);
        CHECK_EQ(again.value("count").toInt(), 2);
    }
    CHECK_EQ(s.vddMatchRefused().at(0).toObject().value("count").toInt(), 1);
    s.clearVddMatchRefusal("GPU A", 330000);
    CHECK_EQ(s.vddMatchRefused().size(), 2);
    CHECK_EQ(s.addVddMatchRefusal("GPU A", 330000, "1.0", 9000), 1);
    s.clearVddMatchRefusal("GPU C", 330000);
    CHECK_EQ(s.vddMatchRefused().size(), 3);
    CHECK_EQ(s.addVddMatchRefusal("GPU A", 0, "1.0", 9000), 0);
    CHECK_EQ(s.vddMatchRefused().size(), 3);
    s.addVddMatchRefusal("GPU A", 330000, "1.1", 9000);
    CHECK_EQ(s.vddMatchRefused().size(), 1);
    CHECK_EQ(s.vddMatchRefused().at(0).toObject().value("version").toString(), QString("1.1"));
    for (int hz = 241; hz < 251; ++hz)
        s.addVddMatchRefusal("GPU A", hz * 1000, "1.1", 9000);
    CHECK_EQ(s.vddMatchRefused().size(), 8);
    CHECK_EQ(s.vddMatchRefused().at(7).toObject().value("milli_hz").toInt(), 250000);
    // The other settings were left alone.
    CHECK_EQ(s.portalRestoreToken(), QString("portal-token-3"));

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

    // ── Statistics reporting ──────────────────────────────────────────────
    // On unless someone says otherwise. Neither census carries a field that
    // identifies anyone, so the switch is the whole rule: no question is put,
    // and an unanswered machine is a reporting machine.
    CHECK_EQ(s.metricsConsentDecision(), QString());
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());

    // One answer, both censuses: the disclosure describes them as one thing.
    s.setMetricsReporting(false);
    CHECK(!s.sessionMetricsAllowed());
    CHECK(!s.updateRelayAllowed());
    s.setMetricsReporting(true);
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());

    // The switch and a hand-edited settings.json are the same control, and
    // each key still silences its own census alone.
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

    // ── Statistics consent, dormant ───────────────────────────────────────
    // The machinery still records what it is given — wording, entry point,
    // timestamp, version — and still refuses to let an answer given to a
    // narrower question cover a wider one. What it no longer does is decide
    // anything: a refusal recorded by an older version does not silence a
    // machine, and a grant does not speak for it either.
    s.setMetricsConsent(false, "the wording that was displayed", "banner");
    CHECK_EQ(s.metricsConsentDecision(), QString("denied"));
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());

    s.setMetricsConsent(true, "the wording that was displayed", "banner");
    CHECK_EQ(s.metricsConsentDecision(), QString("granted"));
    // The record has to say what was agreed to, and when.
    CHECK_EQ(s.metricsConsent().value("message").toString(),
             QString("the wording that was displayed"));
    CHECK_EQ(s.metricsConsent().value("source").toString(), QString("banner"));
    CHECK(!s.metricsConsent().value("at").toString().isEmpty());

    // An answer given to an older, narrower wording still reads as unasked, so
    // the day this goes back in the path it asks again instead of inheriting.
    {
        QJsonObject obj = s.readAll();
        QJsonObject consent = obj["metrics_consent"].toObject();
        consent["version"] = AppSettings::kMetricsConsentVersion - 1;
        obj["metrics_consent"] = consent;
        s.writeAll(obj);
        CHECK_EQ(s.metricsConsentDecision(), QString());
        // …and still reports, because the switch is what decides.
        CHECK(s.sessionMetricsAllowed());
        CHECK(s.updateRelayAllowed());
    }

    // Documented file-only defaults are idempotently seeded.
    s.seedDocumentedDefaults();
    s.audioTimeStretch(); // exercised (value documented as true by default)
    CHECK(s.readAll().contains("native_host_enabled"));

    // keyboard_layout_fidelity: three values, "auto" by default. A boolean from
    // 0.4.0 or before cannot tell a choice from the seed (false since 19/09),
    // so false moves to the new default, and true to the value that keeps its
    // meaning; the file is rewritten once, and an owner's word is kept.
    {
        QJsonObject obj = s.readAll();
        CHECK_EQ(obj.value("keyboard_layout_fidelity").toString(), QString("auto"));
        CHECK_EQ(s.keyboardLayoutFidelity(), QString("auto"));
        obj["keyboard_layout_fidelity"] = false;
        s.writeAll(obj);
        CHECK_EQ(s.keyboardLayoutFidelity(), QString("auto")); // read before any migration
        s.seedDocumentedDefaults();
        CHECK_EQ(s.readAll().value("keyboard_layout_fidelity").toString(), QString("auto"));
        obj["keyboard_layout_fidelity"] = true;
        s.writeAll(obj);
        CHECK_EQ(s.keyboardLayoutFidelity(), QString("always"));
        s.seedDocumentedDefaults();
        CHECK_EQ(s.readAll().value("keyboard_layout_fidelity").toString(), QString("always"));
        obj["keyboard_layout_fidelity"] = "never";
        s.writeAll(obj);
        s.seedDocumentedDefaults();
        CHECK_EQ(s.keyboardLayoutFidelity(), QString("never"));
        obj["keyboard_layout_fidelity"] = " Always ";
        s.writeAll(obj);
        CHECK_EQ(s.keyboardLayoutFidelity(), QString("always"));
        obj["keyboard_layout_fidelity"] = "sometimes";
        s.writeAll(obj);
        CHECK_EQ(s.keyboardLayoutFidelity(), QString("auto"));
        obj["keyboard_layout_fidelity"] = 1;
        s.writeAll(obj);
        s.seedDocumentedDefaults();
        CHECK_EQ(s.readAll().value("keyboard_layout_fidelity").toString(), QString("auto"));
    }

    // The host's administrator windows stay closed to invited players unless
    // the owner wrote a real true: seeded as false so it can be found, absent
    // reads as false, and nothing short of a JSON true opens them.
    {
        CHECK(s.readAll().contains("share_guests_admin_windows"));
        CHECK_EQ(s.readAll().value("share_guests_admin_windows").toBool(true), false);
        CHECK(!s.shareGuestsAdminWindows());
        QJsonObject obj = s.readAll();
        obj["share_guests_admin_windows"] = true;
        s.writeAll(obj);
        CHECK(s.shareGuestsAdminWindows());
        // The seed never takes the owner's choice back.
        s.seedDocumentedDefaults();
        CHECK(s.shareGuestsAdminWindows());
        obj["share_guests_admin_windows"] = "true";
        s.writeAll(obj);
        CHECK(!s.shareGuestsAdminWindows());
        obj["share_guests_admin_windows"] = 1;
        s.writeAll(obj);
        CHECK(!s.shareGuestsAdminWindows());
        obj.remove("share_guests_admin_windows");
        s.writeAll(obj);
        CHECK(!s.shareGuestsAdminWindows());
        s.seedDocumentedDefaults();
    }

    // A booth lets no guest in unless the owner wrote a real true (issue #40):
    // seeded as false, absent reads as false, a string or 1 is not a decision.
    {
        CHECK(s.readAll().contains("share_in_booth_mode"));
        CHECK_EQ(s.readAll().value("share_in_booth_mode").toBool(true), false);
        CHECK(!s.shareInBoothMode());
        QJsonObject obj = s.readAll();
        obj["share_in_booth_mode"] = true;
        s.writeAll(obj);
        CHECK(s.shareInBoothMode());
        s.seedDocumentedDefaults();
        CHECK(s.shareInBoothMode());
        obj["share_in_booth_mode"] = "true";
        s.writeAll(obj);
        CHECK(!s.shareInBoothMode());
        obj["share_in_booth_mode"] = 1;
        s.writeAll(obj);
        CHECK(!s.shareInBoothMode());
        obj.remove("share_in_booth_mode");
        s.writeAll(obj);
        CHECK(!s.shareInBoothMode());
        s.seedDocumentedDefaults();
    }

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

    // ── Router ports (sticky UPnP allocation) ─────────────────────────────
    // Absent key = nothing remembered, on every axis.
    {
        QJsonObject obj = s.readAll();
        obj.remove("router_ports");
        s.writeAll(obj);
    }
    CHECK(s.rememberedTunnelPorts().isEmpty());
    CHECK_EQ(s.rememberedMediaPort(0), quint16(0));
    CHECK_EQ(s.routerLanIp(), QString());

    // Tunnel ports keep the order they were obtained in, without duplicates.
    s.rememberTunnelPort(3478);
    s.rememberTunnelPort(5349);
    s.rememberTunnelPort(3478);
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{3478, 5349}));
    s.forgetTunnelPort(3478);
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));
    s.forgetTunnelPort(4242); // never held: a no-op, not an error
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));
    s.rememberTunnelPort(0); // never a port
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));

    // Media ports are per slot, and forgetting one leaves the others alone.
    s.rememberMediaPort(0, 48010);
    s.rememberMediaPort(1, 46100);
    CHECK_EQ(s.rememberedMediaPort(0), quint16(48010));
    CHECK_EQ(s.rememberedMediaPort(1), quint16(46100));
    CHECK_EQ(s.rememberedMediaPort(2), quint16(0));
    s.forgetMediaPort(0);
    CHECK_EQ(s.rememberedMediaPort(0), quint16(0));
    CHECK_EQ(s.rememberedMediaPort(1), quint16(46100));

    // The LAN address the ports were obtained under, and the three live in
    // one object so a reader of settings.json sees them together.
    s.setRouterLanIp("192.168.1.20");
    CHECK_EQ(s.routerLanIp(), QString("192.168.1.20"));
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));
    {
        const QJsonObject rp = s.readAll().value("router_ports").toObject();
        CHECK(rp.contains("lan_ip"));
        CHECK(rp.contains("tunnel"));
        CHECK(rp.contains("media"));
    }
    // Not seeded: it is state, not a preference to discover in the file.
    {
        QJsonObject obj = s.readAll();
        obj.remove("router_ports");
        s.writeAll(obj);
        s.seedDocumentedDefaults();
        CHECK(!s.readAll().contains("router_ports"));
    }

    // The admin's codec of a native session is gone (PyroWave is each
    // browser's choice now): a file that still names it loses the key at the
    // next start, and nothing else in it moves.
    {
        QJsonObject obj = s.readAll();
        obj["native_video_codec"] = "pyrowave";
        s.writeAll(obj);
        s.seedDocumentedDefaults();
        const QJsonObject after = s.readAll();
        CHECK(!after.contains("native_video_codec"));
        CHECK_EQ(after.value("native_video_pipeline").toString(),
                 obj.value("native_video_pipeline").toString());
        CHECK_EQ(after.value("native_host_enabled").toBool(), true);
    }

    // The bench's engine knobs: absent, empty and never seeded; read trimmed
    // once hand-added.
    CHECK(s.nativeTuning().isEmpty());
    {
        s.seedDocumentedDefaults();
        CHECK(!s.readAll().contains("native_tuning"));
        QJsonObject obj = s.readAll();
        obj["native_tuning"] = " namedrops=1 ";
        s.writeAll(obj);
        CHECK_EQ(s.nativeTuning(), QString("namedrops=1"));
        obj.remove("native_tuning");
        s.writeAll(obj);
        CHECK(s.nativeTuning().isEmpty());
    }

    // The administrator's ceilings (issue #53): seeded at 0, read in every
    // form Bruno's examples give, and anything else is no limit.
    {
        s.seedDocumentedDefaults();
        const QJsonObject seeded = s.readAll();
        CHECK_EQ(seeded.value("max_allowed_frame_rate").toInt(-1), 0);
        CHECK_EQ(seeded.value("max_allowed_bitrate").toInt(-1), 0);
        CHECK_EQ(seeded.value("max_allowed_stream_resolution").toString(), QString("0x0"));
        CHECK(!s.streamLimits().any());

        int w = -1, h = -1;
        CHECK(AppSettings::parseStreamResolution("1920x1080", &w, &h));
        CHECK(w == 1920 && h == 1080);
        CHECK(AppSettings::parseStreamResolution("1920", &w, &h));
        CHECK(w == 1920 && h == 1920);
        CHECK(AppSettings::parseStreamResolution(" 0x1440 ", &w, &h));
        CHECK(w == 0 && h == 1440);
        CHECK(AppSettings::parseStreamResolution("0", &w, &h));
        CHECK(w == 0 && h == 0);
        CHECK(AppSettings::parseStreamResolution("0x0", &w, &h));
        CHECK(w == 0 && h == 0);
        CHECK(AppSettings::parseStreamResolution("2560X1440", &w, &h));
        CHECK(w == 2560 && h == 1440);
        for (const char* bad : {"", "x", "1920x", "1920*1080", "-1", "1080p", "1920x1080x2"})
            CHECK(!AppSettings::parseStreamResolution(bad, &w, &h));

        QJsonObject obj = s.readAll();
        obj["max_allowed_frame_rate"] = 60;
        obj["max_allowed_bitrate"] = "40";
        obj["max_allowed_stream_resolution"] = 1920;
        s.writeAll(obj);
        AppSettings::StreamLimits limits = s.streamLimits();
        CHECK_EQ(limits.fps, 60);
        CHECK_EQ(limits.bitrateMbps, 40);
        CHECK(limits.width == 1920 && limits.height == 1920);
        obj["max_allowed_frame_rate"] = -5;
        obj["max_allowed_bitrate"] = "a lot";
        obj["max_allowed_stream_resolution"] = "big";
        s.writeAll(obj);
        CHECK(!s.streamLimits().any());

        s.setMaxAllowedFrameRate(30);
        s.setMaxAllowedBitrate(15);
        s.setMaxAllowedStreamResolution(0, 1080);
        CHECK_EQ(s.readAll().value("max_allowed_stream_resolution").toString(), QString("0x1080"));
        limits = s.streamLimits();
        CHECK(limits.fps == 30 && limits.bitrateMbps == 15 && limits.width == 0 &&
              limits.height == 1080);
        // A hand edit of the file stays as it was written at the next start.
        s.seedDocumentedDefaults();
        CHECK_EQ(s.streamLimits().fps, 30);
        obj = s.readAll();
        obj.remove("max_allowed_frame_rate");
        obj.remove("max_allowed_bitrate");
        obj.remove("max_allowed_stream_resolution");
        s.writeAll(obj);
    }

    // The size a launch keeps under the resolution ceiling.
    {
        AppSettings::StreamLimits box;
        box.width = 1920;
        box.height = 1080;
        // An explicit size keeps its shape.
        auto r = AppSettings::limitStreamSize(box, 2560, 1440, false, false, false, 0, 0);
        CHECK(r.changed && r.width == 1920 && r.height == 1080 && !r.fitBox);
        r = AppSettings::limitStreamSize(box, 2560, 1080, false, false, false, 0, 0);
        CHECK(r.width == 1920 && r.height == 810);
        // Under it: untouched.
        r = AppSettings::limitStreamSize(box, 1280, 720, false, false, false, 0, 0);
        CHECK(!r.changed && r.width == 1280 && r.height == 720);
        // A portrait phone's own size, literally inside 1920x1080.
        r = AppSettings::limitStreamSize(box, 1170, 2532, false, false, false, 0, 0);
        CHECK(r.height == 1080 && r.width == 498);
        // The native host's box is clipped side by side; "Match my screen"
        // keeps its shape, its fallback box is clipped.
        r = AppSettings::limitStreamSize(box, 2560, 1600, true, false, true, 0, 0);
        CHECK(r.fitBox && r.width == 1920 && r.height == 1080);
        r = AppSettings::limitStreamSize(box, 2560, 1600, true, true, true, 2560, 1440);
        CHECK(r.width == 1728 && r.height == 1080 && r.fallbackWidth == 1920 &&
              r.fallbackHeight == 1080);
        // Its own size becomes the ceiling's box; its fixed height, a box as
        // wide as the ceiling.
        r = AppSettings::limitStreamSize(box, 0, 0, false, false, true, 0, 0);
        CHECK(r.fitBox && r.width == 1920 && r.height == 1080);
        r = AppSettings::limitStreamSize(box, 1920, 1080, false, false, true, 0, 0);
        CHECK(r.changed && r.fitBox && r.width == 1920 && r.height == 1080);
        // The height alone: a free width.
        AppSettings::StreamLimits tall;
        tall.height = 1440;
        r = AppSettings::limitStreamSize(tall, 3840, 2160, false, false, true, 0, 0);
        CHECK(!r.fitBox && r.width == 2560 && r.height == 1440);
        r = AppSettings::limitStreamSize(tall, 0, 0, false, false, true, 0, 0);
        CHECK(r.fitBox && r.width == AppSettings::kLimitMax && r.height == 1440);
        // No ceiling: nothing moves.
        r = AppSettings::limitStreamSize({}, 0, 0, false, false, true, 0, 0);
        CHECK(!r.changed && r.width == 0 && r.height == 0 && !r.fitBox);
    }

    // Low-level access.
    QJsonObject all = s.readAll();
    CHECK(all.contains("http_port"));
    all["custom_key"] = 99;
    s.writeAll(all);
    CHECK_EQ(s.readAll().value("custom_key").toInt(), 99);

    // The native host's display cards keep their ids. The N95 of 16/10/2026:
    // our virtual display, made primary, went first on the Intel GPU, and
    // "Display 1" moved from rank 0 to rank 1 — its card's id with it.
    {
        const QString display1 = "\\\\?\\DISPLAY#BNQ7F6B#5&1d3b2c2&0&UID4352#{e6f07b5f}";
        const QString vdd = "\\\\?\\DISPLAY#MTT1337#1&15ecd195&0&UID256#{e6f07b5f}";
        // First seen: each gets the id its card had as a rank (rank + 1).
        CHECK(s.displayAppIds({display1, vdd}, {1, 2}) == (QList<int>{1, 2}));
        // Ours turns on and goes first (no card numbered here: empty key):
        // the others keep theirs at their new ranks.
        CHECK(s.displayAppIds({QString(), display1, vdd}, {1, 2, 3}) == (QList<int>{0, 1, 2}));
        // Any case: Windows promises none for a device path.
        CHECK(s.displayAppIds({vdd.toUpper(), display1}, {1, 2}) == (QList<int>{2, 1}));
        // A new screen whose rank id is taken gets the lowest free one; one
        // unplugged keeps its id for when it comes back.
        CHECK(s.displayAppIds({display1, "dp-3", vdd}, {1, 2, 3}) == (QList<int>{1, 3, 2}));
        CHECK(s.displayAppIds({display1, vdd}, {1, 2}) == (QList<int>{1, 2}));
        CHECK(s.displayAppIds({"dp-3"}, {1}) == (QList<int>{3}));
        CHECK_EQ(s.readAll().value("display_app_ids").toObject().size(), 3);
        // A kept id wins over whatever rank the screen has now.
        CHECK(s.displayAppIds({display1}, {9}) == (QList<int>{1}));
        // Every id taken: a screen not here gives its own up, the lowest.
        QJsonObject full;
        for (int id = 1; id < AppSettings::kDisplayAppIdEnd; ++id)
            full[QString("screen-%1").arg(id)] = id;
        QJsonObject file = s.readAll();
        file["display_app_ids"] = full;
        s.writeAll(file);
        CHECK(s.displayAppIds({"screen-1", "new"}, {1, 2}) == (QList<int>{1, 2}));
        CHECK(!s.readAll().value("display_app_ids").toObject().contains("screen-2"));
        file.remove("display_app_ids");
        s.writeAll(file);
    }

    // FQDN validation.
    CHECK(AppSettings::isValidFqdn("host.example.com"));
    CHECK(AppSettings::isValidFqdn("a.b"));
    CHECK(!AppSettings::isValidFqdn("nodot"));
    CHECK(!AppSettings::isValidFqdn(".leadingdot.com"));
    CHECK(!AppSettings::isValidFqdn("trailingdot."));
    CHECK(!AppSettings::isValidFqdn(""));

    // --config: every instance built afterwards uses that file, made absolute,
    // its directory created; an empty path goes back to the data directory.
    {
        const QString defaultPath = AppSettings().filePath();
        const QString custom = tmp.path() + "/managed/conf/settings.json";
        AppSettings::setFileOverride(custom);
        CHECK_EQ(AppSettings::fileOverride(), QFileInfo(custom).absoluteFilePath());
        AppSettings managed;
        CHECK_EQ(managed.filePath(), QFileInfo(custom).absoluteFilePath());
        CHECK(QFileInfo(tmp.path() + "/managed/conf").isDir());
        managed.setHttpsPort(9443);
        CHECK_EQ(AppSettings().httpsPort(1), quint16(9443));
        CHECK_EQ(s.httpsPort(1), quint16(443)); // built before: its own file

        AppSettings::setFileOverride("relative.json");
        CHECK(QFileInfo(AppSettings::fileOverride()).isAbsolute());

        AppSettings::setFileOverride(QString());
        CHECK(AppSettings::fileOverride().isEmpty());
        CHECK_EQ(AppSettings().filePath(), defaultPath);
    }

    SECTION("AppSettings: a file that cannot be read is never written over");
    {
        AppSettings h;
        h.m_FilePath = tmp.path() + "/held/settings.json";
        QDir().mkpath(tmp.path() + "/held");
        h.setHttpPort(81);
        const QString key = h.localKey();
        const QByteArray signing = h.hostSigningKeyPem();
        CHECK(!key.isEmpty());
        CHECK(!signing.isEmpty());
        bool complete = false;
        CHECK_EQ(h.readAll(&complete).value("http_port").toInt(), 81);
        CHECK(complete);
        // The date host sessions are older than: written once, then read back.
        const qint64 before = h.hostSessionRepairBefore();
        CHECK(before > 0);
        CHECK(before <= QDateTime::currentSecsSinceEpoch());
        CHECK_EQ(h.readAll(&complete).value("host_session_repair_before").toInteger(), before);
        CHECK_EQ(h.hostSessionRepairBefore(), before);

        bool held = false;
        {
            FileHold hold(h.m_FilePath);
            held = hold.held();
            if (held) {
                h.setHttpsPort(9443);          // not written
                CHECK(h.localKey().isEmpty()); // none made in its place
                CHECK(h.hostSigningKeyPem().isEmpty());
                CHECK(h.rotateLocalKey().isEmpty());
                CHECK_EQ(h.hostSessionRepairBefore(), qint64(0)); // none made in its place
                h.readAll(&complete);
                CHECK(!complete);
            } else {
                Logger::info("[test] the file could not be held here: skipping the held part");
            }
        }
        if (held) {
            // The file reads again by the time the write comes: what that read
            // missed is still not written over it.
            QJsonObject one;
            one["https_port"] = 9443;
            h.writeAll(one);
        }

        CHECK_EQ(h.httpPort(1), quint16(81));
        CHECK_EQ(h.httpsPort(1), quint16(1));
        CHECK_EQ(h.localKey(), key);
        CHECK(h.hostSigningKeyPem() == signing);
        CHECK_EQ(h.hostSessionRepairBefore(), before);
        h.setHttpsPort(9443); // and written again once it reads
        CHECK_EQ(h.httpsPort(1), quint16(9443));
        CHECK_EQ(h.httpPort(1), quint16(81));

        // A missing file and an empty one hold nothing: read whole, written freely.
        AppSettings e;
        e.m_FilePath = tmp.path() + "/held/none.json";
        e.readAll(&complete);
        CHECK(complete);
        QFile empty(e.m_FilePath);
        CHECK(empty.open(QIODevice::WriteOnly));
        empty.close();
        e.readAll(&complete);
        CHECK(complete);
        e.setHttpPort(82);
        CHECK_EQ(e.httpPort(1), quint16(82));
    }

    SECTION("AppSettings: a file that is not a settings document is kept aside");
    {
        const QString dir = tmp.path() + "/bad";
        QDir().mkpath(dir);
        AppSettings b;
        b.m_FilePath = dir + "/settings.json";
        for (const QByteArray& content :
             {QByteArray("{\"http_port\": 81,"), QByteArray("[1, 2]")}) {
            for (const QFileInfo& old : QDir(dir).entryInfoList(QDir::Files))
                QFile::remove(old.absoluteFilePath());
            QFile f(b.m_FilePath);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write(content);
            f.close();

            bool complete = false;
            CHECK(b.readAll(&complete).isEmpty());
            CHECK(complete);
            const QStringList aside =
                QDir(dir).entryList({"settings.json.unreadable-*"}, QDir::Files);
            CHECK_EQ(aside.size(), 1);
            if (!aside.isEmpty()) {
                QFile kept(dir + "/" + aside.first());
                CHECK(kept.open(QIODevice::ReadOnly));
                CHECK(kept.readAll() == content);
            }
            b.setHttpPort(83); // starts from the defaults
            CHECK_EQ(b.httpPort(1), quint16(83));
        }
    }
}
