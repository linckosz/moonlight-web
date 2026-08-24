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

#include "NvComputer.h"

#include "server/NetClassify.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkInterface>

#include <algorithm>

// --- Construction from serverInfo XML ---------------------------------------

NvComputer::NvComputer(const QString& serverInfo, const NvAddress& activeAddr)
    : state(CS_ONLINE)
    , activeAddress(activeAddr)
{
    // Parse basic string fields
    name = NvHTTP::getXmlString(serverInfo, "hostname");
    if (name.isEmpty()) name = "UNKNOWN";

    uuid = NvHTTP::getXmlString(serverInfo, "uniqueid");

    // MAC address: parse colon-separated hex
    QString macStr = NvHTTP::getXmlString(serverInfo, "mac");
    if (!macStr.isEmpty() && macStr != "00:00:00:00:00:00") {
        QStringList bytes = macStr.split(':');
        for (const QString& b : bytes)
            macAddress.append(static_cast<char>(b.toInt(nullptr, 16)));
    }

    // Pair status
    pairState = pairStateFromString(NvHTTP::getXmlString(serverInfo, "PairStatus"));

    // Server state (busy detection)
    QString serverState = NvHTTP::getXmlString(serverInfo, "state");
    isNvidiaServerSoftware = (!serverState.isEmpty() && serverState.contains("MJOLNIR"));
    currentGameId = NvHTTP::getCurrentGame(serverInfo);

    // Version info
    gfeVersion = NvHTTP::getXmlString(serverInfo, "GfeVersion");
    appVersion = NvHTTP::getXmlString(serverInfo, "appversion");
    gpuModel = NvHTTP::getXmlString(serverInfo, "gputype");

    // Codec support
    QString codecStr = NvHTTP::getXmlString(serverInfo, "ServerCodecModeSupport");
    serverCodecModeSupport = codecStr.isEmpty() ? 1 : codecStr.toInt(); // default H.264

    QString hevcPixels = NvHTTP::getXmlString(serverInfo, "MaxLumaPixelsHEVC");
    maxLumaPixelsHEVC = hevcPixels.isEmpty() ? 0 : hevcPixels.toInt();

    // Display modes
    displayModes = NvHTTP::getDisplayModeList(serverInfo);

    // Network addresses
    QString localIp = NvHTTP::getXmlString(serverInfo, "LocalIP");
    if (!localIp.isEmpty() && !localIp.startsWith("127.")) {
        localAddress = NvAddress(localIp, activeAddr.port());
    }

    QString externalIp = NvHTTP::getXmlString(serverInfo, "ExternalIP");
    if (!externalIp.isEmpty()) {
        quint16 extPort = NvHTTP::getXmlString(serverInfo, "ExternalPort").toUShort();
        if (extPort == 0) extPort = activeAddr.port();
        remoteAddress = NvAddress(externalIp, extPort);
    }

    // HTTPS port for pairing
    QString httpsPortStr = NvHTTP::getXmlString(serverInfo, "HttpsPort");
    activeHttpsPort = httpsPortStr.isEmpty() ? 0 : httpsPortStr.toUShort();
    if (activeHttpsPort == 0) activeHttpsPort = MW_HTTPS_PORT; // Sunshine HTTPS port fallback
}

// --- Construction from QSettings --------------------------------------------

NvComputer::NvComputer(QSettings& settings)
    : state(CS_OFFLINE) // persisted hosts start offline
{
    name = settings.value("hostname").toString();
    uuid = settings.value("uuid").toString();
    macAddress = settings.value("mac").toByteArray();

    // The address that last answered, restored first: it is the only candidate we
    // have actually seen work. Without it a restart falls back to the host's
    // self-reported <LocalIP>, which on a multi-homed machine (podman/virbr0/VPN)
    // or after a DHCP change names a path that never answers — and polling never
    // tries a second candidate on its own.
    activeAddress = NvAddress(settings.value("activeaddress").toString(),
                              settings.value("activeport", MW_HTTP_PORT).toUInt());

    localAddress = NvAddress(settings.value("localaddress").toString(),
                             settings.value("localport", MW_HTTP_PORT).toUInt());

    remoteAddress = NvAddress(settings.value("remoteaddress").toString(),
                              settings.value("remoteport", MW_HTTP_PORT).toUInt());

    manualAddress = NvAddress(settings.value("manualaddress").toString(),
                              settings.value("manualport", MW_HTTP_PORT).toUInt());

    serverCertPem = settings.value("serverCert").toByteArray();
    activeHttpsPort = static_cast<quint16>(settings.value("httpsPort", MW_HTTPS_PORT).toUInt());

    pairState = pairStateFromString(settings.value("pairState").toString());

    backendType = settings.value("backendType").toString();
    backendApiUrl = settings.value("backendApiUrl").toString();
    backendApiToken = settings.value("backendApiToken").toString();
    backendPairUser = settings.value("backendPairUser").toString();
    backendPairPassword = settings.value("backendPairPassword").toString();
}

// --- Serialization ----------------------------------------------------------

void NvComputer::serialize(QSettings& settings) const
{
    settings.setValue("hostname", name);
    settings.setValue("uuid", uuid);
    settings.setValue("mac", macAddress);
    settings.setValue("activeaddress", activeAddress.address());
    settings.setValue("activeport", activeAddress.port());
    settings.setValue("localaddress", localAddress.address());
    settings.setValue("localport", localAddress.port());
    settings.setValue("remoteaddress", remoteAddress.address());
    settings.setValue("remoteport", remoteAddress.port());
    settings.setValue("manualaddress", manualAddress.address());
    settings.setValue("manualport", manualAddress.port());
    settings.setValue("serverCert", serverCertPem);
    settings.setValue("httpsPort", static_cast<quint32>(activeHttpsPort));
    settings.setValue("pairState", pairStateToString(pairState));
    settings.setValue("backendType", backendType);
    settings.setValue("backendApiUrl", backendApiUrl);
    settings.setValue("backendApiToken", backendApiToken);
    settings.setValue("backendPairUser", backendPairUser);
    settings.setValue("backendPairPassword", backendPairPassword);
}

bool NvComputer::isEqualSerialized(const NvComputer& that) const
{
    return name == that.name && uuid == that.uuid && macAddress == that.macAddress &&
           activeAddress == that.activeAddress && localAddress == that.localAddress &&
           remoteAddress == that.remoteAddress &&
           manualAddress == that.manualAddress && serverCertPem == that.serverCertPem &&
           activeHttpsPort == that.activeHttpsPort;
}

// --- Merge polling data -----------------------------------------------------

bool NvComputer::update(const NvComputer& that)
{
    // UUID must match
    if (this->uuid != that.uuid) return false;

    bool changed = false;

#define ASSIGN_IF_CHANGED(f)                                                                       \
    if (this->f != that.f) {                                                                       \
        this->f = that.f;                                                                          \
        changed = true;                                                                            \
    }

    ASSIGN_IF_CHANGED(state)
    ASSIGN_IF_CHANGED(activeAddress)
    ASSIGN_IF_CHANGED(currentGameId)
    ASSIGN_IF_CHANGED(serverCodecModeSupport)
    ASSIGN_IF_CHANGED(maxLumaPixelsHEVC)
    ASSIGN_IF_CHANGED(isNvidiaServerSoftware)
    ASSIGN_IF_CHANGED(activeHttpsPort)

#undef ASSIGN_IF_CHANGED

#define ASSIGN_IF_CHANGED_NONEMPTY(f)                                                              \
    if (!that.f.isEmpty() && this->f != that.f) {                                                  \
        this->f = that.f;                                                                          \
        changed = true;                                                                            \
    }

    ASSIGN_IF_CHANGED_NONEMPTY(gfeVersion)
    ASSIGN_IF_CHANGED_NONEMPTY(appVersion)
    ASSIGN_IF_CHANGED_NONEMPTY(gpuModel)
    ASSIGN_IF_CHANGED_NONEMPTY(name)

    if (!that.macAddress.isEmpty() && this->macAddress != that.macAddress) {
        this->macAddress = that.macAddress;
        changed = true;
    }

    // Follow the host's own <LocalIP> whenever it moves. Freezing the first one
    // ever seen outlived its usefulness the moment it could be wrong: a DHCP
    // lease change or a Sunshine bound to another interface would otherwise
    // strand the host on a dead address for good. activeAddress still wins the
    // poll, so a working host never changes path because of this.
    if (!that.localAddress.isNull() && this->localAddress != that.localAddress) {
        this->localAddress = that.localAddress;
        changed = true;
    }

    if (this->remoteAddress.isNull() && !that.remoteAddress.isNull()) {
        this->remoteAddress = that.remoteAddress;
        changed = true;
    }

    if (!that.displayModes.isEmpty() && this->displayModes != that.displayModes) {
        this->displayModes = that.displayModes;
        changed = true;
    }

    // The address an operator typed in. Nothing else writes it, so without this
    // merge "Add manually" only ever lived in activeAddress and was lost at the
    // next restart.
    if (!that.manualAddress.isNull() && this->manualAddress != that.manualAddress) {
        this->manualAddress = that.manualAddress;
        changed = true;
    }

    // Merge server cert from polling (never cleared by polling)
    if (!that.serverCertPem.isEmpty() && this->serverCertPem != that.serverCertPem) {
        this->serverCertPem = that.serverCertPem;
        changed = true;
    }

    return changed;
}

// --- Unique addresses for polling -------------------------------------------

namespace {

/// Ordering key for the candidate addresses: the less exposed the path, the
/// earlier it is tried. The MoonlightWeb ↔ Sunshine link carries the GameStream
/// pairing and /serverinfo in cleartext on port 47989, so whenever both a
/// private (or mesh-VPN) and a routable address are known for the same host, the
/// cleartext exchange should not be the one crossing the internet.
/// An address that does not parse as an IP — a hostname — ranks with the public
/// ones: we cannot tell where it leads.
int exposureRank(const NvAddress& na)
{
    switch (NetClassify::classify(na.address())) {
    case NetClassify::Kind::Loopback: return 0;
    case NetClassify::Kind::Private: return 1;
    case NetClassify::Kind::Tunnel: return 2;
    case NetClassify::Kind::Public: return 3;
    }
    return 3;
}

} // namespace

QVector<NvAddress> NvComputer::uniqueAddresses() const
{
    QVector<NvAddress> addrs;

    // Active address first — this is the address that last responded successfully
    // (updated in onPollReplyFinished from the URL used in the poll request, and
    // persisted, so a restart resumes on it rather than on <LocalIP>).
    // It is more reliable than localAddress (from XML <LocalIP>) which may differ
    // from the mDNS-resolved address in multi-homed / Docker / VPN setups.
    //
    // Deliberately not demoted, even when a less exposed candidate exists: it is
    // the one path we have seen answer, so moving it down would cost a restarted
    // host its first tick — and, for a host reachable only the "worse" way, more
    // than that. Polling walks the rest of the list when it stops answering.
    if (!activeAddress.isNull()) addrs.append(activeAddress);

    // The rest, least exposed first — this is what decides the initial pick, and
    // any pick made after the active address is cleared.
    QVector<NvAddress> rest;
    if (!localAddress.isNull()) rest.append(localAddress);
    if (!manualAddress.isNull()) rest.append(manualAddress);
    if (!remoteAddress.isNull()) rest.append(remoteAddress);

    std::stable_sort(rest.begin(), rest.end(), [](const NvAddress& a, const NvAddress& b) {
        return exposureRank(a) < exposureRank(b);
    });

    // Distinct paths only, as the name promises: activeAddress is usually equal
    // to one of the others, and a duplicate would cost the poll a whole tick
    // retrying the address that just failed.
    for (const NvAddress& na : rest)
        if (!addrs.contains(na)) addrs.append(na);

    return addrs;
}

// --- Local-machine detection ------------------------------------------------

bool NvComputer::isLocalMachine() const
{
    // Same logic the setup wizard uses (SystemRoutes): a host is "us" when any of
    // its addresses is localhost, loopback, or an address bound to one of this
    // machine's own network interfaces (mDNS may register the local Sunshine
    // under its LAN IP rather than 127.0.0.1).
    const QList<QHostAddress> selfAddrs = QNetworkInterface::allAddresses();
    for (const NvAddress& na : {activeAddress, localAddress, manualAddress}) {
        if (na.isNull()) continue;
        const QString addrStr = na.address();
        if (addrStr.isEmpty()) continue;
        if (addrStr.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) return true;
        const QHostAddress ip(addrStr);
        if (ip.isNull()) continue;
        if (ip.isLoopback()) return true;
        if (selfAddrs.contains(ip)) return true;
    }
    return false;
}

// --- JSON serialization -----------------------------------------------------

// Deliberately address-free: this is the API representation, and the browser
// never dials a host directly — every route is keyed by uuid and the streaming
// path goes through this server. Exposing the LAN topology (addresses, MAC) to
// any page that can reach /api/hosts buys the frontend nothing, so it is not
// sent. Persistence uses serialize(QSettings&), not this, so the addresses are
// still remembered across restarts.
QJsonObject NvComputer::toJson() const
{
    QJsonObject obj;
    obj["uuid"] = uuid;
    obj["name"] = name;
    obj["state"] = computerStateToString(state);
    obj["reachable"] = reachable;
    obj["pairState"] = pairStateToString(pairState);
    obj["port"] = static_cast<int>(activeAddress.port());
    obj["gpuModel"] = gpuModel;
    obj["gfeVersion"] = gfeVersion;
    obj["appVersion"] = appVersion;
    obj["currentGameId"] = currentGameId;
    obj["serverCodecModeSupport"] = serverCodecModeSupport;
    obj["maxLumaPixelsHEVC"] = maxLumaPixelsHEVC;

    // Frontend uses this to warn when a user streams the very PC they're on.
    obj["isLocalHost"] = isLocalMachine();

    // Which backend drives this host, so the UI can offer backend management on
    // the hosts that have one and leave a plain Sunshine card untouched. The
    // token is deliberately absent: it is a secret the browser never needs, and
    // the settings dialog sends a new one rather than editing the old.
    obj["backendType"] = backendType;
    obj["backendApiUrl"] = backendApiUrl;
    obj["backendConfigured"] = !backendApiToken.isEmpty();
    // The user name is not a secret and the dialog needs it to show what is
    // stored; the password only ever surfaces as a boolean.
    obj["backendPairUser"] = backendPairUser;
    obj["backendPairConfigured"] = !backendPairPassword.isEmpty();

    // A MultiSeat control API answered on this host. The UI offers its setup
    // only when this is true, which is how a plain Sunshine card stays plain
    // without anyone having to declare that it is one.
    obj["multiSeatDetected"] = multiSeatApiPresent;

    // Wake-on-LAN is sent by the server (POST /api/hosts/:id/wol), so the MAC
    // itself never has to reach the browser — the UI only needs to know whether
    // the button can be offered at all.
    obj["wakeSupported"] =
        !macAddress.isEmpty() && macAddress != QByteArray(macAddress.size(), '\0');

    // Display modes
    QJsonArray modesArr;
    for (const auto& mode : displayModes) {
        QJsonObject modeObj;
        modeObj["width"] = mode.width;
        modeObj["height"] = mode.height;
        modeObj["refreshRate"] = mode.refreshRate;
        modesArr.append(modeObj);
    }
    obj["displayModes"] = modesArr;

    return obj;
}

// --- State conversions ------------------------------------------------------

NvComputer::PairState NvComputer::pairStateFromString(const QString& s)
{
    // Accept both XML numeric values ("1"/"0") and persisted strings
    if (s == "1" || s == "paired") return PS_PAIRED;
    if (s == "0" || s == "not_paired") return PS_NOT_PAIRED;
    return PS_UNKNOWN;
}

QString NvComputer::pairStateToString(PairState ps)
{
    switch (ps) {
    case PS_PAIRED: return "paired";
    case PS_NOT_PAIRED: return "not_paired";
    default: return "unknown";
    }
}

QString NvComputer::computerStateToString(ComputerState cs)
{
    switch (cs) {
    case CS_ONLINE: return "online";
    case CS_OFFLINE: return "offline";
    default: return "unknown";
    }
}
