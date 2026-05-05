#include "NvComputer.h"

#include <QJsonObject>
#include <QJsonArray>

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
    serverCodecModeSupport = codecStr.isEmpty() ? 1 : codecStr.toInt();  // default H.264

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
    if (activeHttpsPort == 0)
        activeHttpsPort = MW_HTTPS_PORT;
}

// --- Construction from QSettings --------------------------------------------

NvComputer::NvComputer(QSettings& settings)
    : state(CS_OFFLINE)  // persisted hosts start offline
{
    name = settings.value("hostname").toString();
    uuid = settings.value("uuid").toString();
    macAddress = settings.value("mac").toByteArray();

    localAddress = NvAddress(
        settings.value("localaddress").toString(),
        settings.value("localport", 0).toUInt());

    remoteAddress = NvAddress(
        settings.value("remoteaddress").toString(),
        settings.value("remoteport", 0).toUInt());

    manualAddress = NvAddress(
        settings.value("manualaddress").toString(),
        settings.value("manualport", 0).toUInt());

    serverCertPem = settings.value("serverCert").toByteArray();
    activeHttpsPort = static_cast<quint16>(
        settings.value("httpsPort", MW_HTTPS_PORT).toUInt());

    pairState = pairStateFromString(settings.value("pairState").toString());
}

// --- Serialization ----------------------------------------------------------

void NvComputer::serialize(QSettings& settings) const
{
    settings.setValue("hostname", name);
    settings.setValue("uuid", uuid);
    settings.setValue("mac", macAddress);
    settings.setValue("localaddress", localAddress.address());
    settings.setValue("localport", localAddress.port());
    settings.setValue("remoteaddress", remoteAddress.address());
    settings.setValue("remoteport", remoteAddress.port());
    settings.setValue("manualaddress", manualAddress.address());
    settings.setValue("manualport", manualAddress.port());
    settings.setValue("serverCert", serverCertPem);
    settings.setValue("httpsPort", static_cast<quint32>(activeHttpsPort));
    settings.setValue("pairState", pairStateToString(pairState));
}

bool NvComputer::isEqualSerialized(const NvComputer& that) const
{
    return name == that.name
        && uuid == that.uuid
        && macAddress == that.macAddress
        && localAddress == that.localAddress
        && remoteAddress == that.remoteAddress
        && manualAddress == that.manualAddress
        && serverCertPem == that.serverCertPem
        && activeHttpsPort == that.activeHttpsPort;
}

// --- Merge polling data -----------------------------------------------------

bool NvComputer::update(const NvComputer& that)
{
    // UUID must match
    if (this->uuid != that.uuid)
        return false;

    bool changed = false;

#define ASSIGN_IF_CHANGED(f) \
    if (this->f != that.f) { this->f = that.f; changed = true; }

    ASSIGN_IF_CHANGED(state)
    ASSIGN_IF_CHANGED(activeAddress)
    ASSIGN_IF_CHANGED(currentGameId)
    ASSIGN_IF_CHANGED(serverCodecModeSupport)
    ASSIGN_IF_CHANGED(maxLumaPixelsHEVC)
    ASSIGN_IF_CHANGED(isNvidiaServerSoftware)
    ASSIGN_IF_CHANGED(activeHttpsPort)

#undef ASSIGN_IF_CHANGED

#define ASSIGN_IF_CHANGED_NONEMPTY(f) \
    if (!that.f.isEmpty() && this->f != that.f) { this->f = that.f; changed = true; }

    ASSIGN_IF_CHANGED_NONEMPTY(gfeVersion)
    ASSIGN_IF_CHANGED_NONEMPTY(appVersion)
    ASSIGN_IF_CHANGED_NONEMPTY(gpuModel)
    ASSIGN_IF_CHANGED_NONEMPTY(name)

    if (!that.macAddress.isEmpty() && this->macAddress != that.macAddress) {
        this->macAddress = that.macAddress;
        changed = true;
    }

    if (this->localAddress.isNull() && !that.localAddress.isNull()) {
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

    // Merge server cert from polling (never cleared by polling)
    if (!that.serverCertPem.isEmpty() && this->serverCertPem != that.serverCertPem) {
        this->serverCertPem = that.serverCertPem;
        changed = true;
    }

    return changed;
}

// --- Unique addresses for polling -------------------------------------------

QVector<NvAddress> NvComputer::uniqueAddresses() const
{
    QVector<NvAddress> addrs;

    if (!localAddress.isNull())
        addrs.append(localAddress);
    if (!manualAddress.isNull())
        addrs.append(manualAddress);
    if (!remoteAddress.isNull())
        addrs.append(remoteAddress);

    // If no address is set, use the active one
    if (addrs.isEmpty() && !activeAddress.isNull())
        addrs.append(activeAddress);

    return addrs;
}

// --- JSON serialization -----------------------------------------------------

QJsonObject NvComputer::toJson() const
{
    QJsonObject obj;
    obj["uuid"]         = uuid;
    obj["name"]         = name;
    obj["state"]        = computerStateToString(state);
    obj["pairState"]    = pairStateToString(pairState);
    obj["activeAddress"] = activeAddress.address();
    obj["port"]         = static_cast<int>(activeAddress.port());
    obj["gpuModel"]     = gpuModel;
    obj["gfeVersion"]   = gfeVersion;
    obj["appVersion"]   = appVersion;
    obj["currentGameId"] = currentGameId;
    obj["serverCodecModeSupport"] = serverCodecModeSupport;
    obj["maxLumaPixelsHEVC"] = maxLumaPixelsHEVC;

    // MAC as hex string
    obj["macAddress"] = QString::fromUtf8(macAddress.toHex(':'));

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

    // Persisted addresses
    obj["localAddress"]  = localAddress.toString();
    obj["remoteAddress"] = remoteAddress.toString();
    obj["manualAddress"] = manualAddress.toString();

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
    case PS_PAIRED:     return "paired";
    case PS_NOT_PAIRED: return "not_paired";
    default:            return "unknown";
    }
}

QString NvComputer::computerStateToString(ComputerState cs)
{
    switch (cs) {
    case CS_ONLINE:  return "online";
    case CS_OFFLINE: return "offline";
    default:         return "unknown";
    }
}
