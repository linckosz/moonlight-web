// Must be defined before any miniupnpc header to prevent dllimport on Windows
#define MINIUPNP_STATICLIB

#include "UPNPClient.h"

#ifdef MW_HAVE_MINIUPNPC
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <miniupnpc/upnpreplyparse.h>
#endif

#include <QDebug>
#include <QHostInfo>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

UPNPClient::UPNPClient(QObject* parent)
    : QObject(parent)
{
    qInfo() << "[UPNP] Client created";
}

UPNPClient::~UPNPClient()
{
    qInfo() << "[UPNP] Destructor";
    cleanup();
}

bool UPNPClient::discover(int timeoutMs)
{
#ifndef MW_HAVE_MINIUPNPC
    qWarning() << "[UPNP] miniupnpc not available — UPnP disabled";
    emit error("miniupnpc not compiled in");
    return false;
#else
    if (m_Available) {
        qInfo() << "[UPNP] Already discovered, cleaning up first";
        cleanup();
    }

    qInfo() << "[UPNP] Discovering UPnP devices on LAN (timeout=" << timeoutMs << "ms)";

    struct UPNPDev* devlist = nullptr;
    int discoverError = 0;

    devlist = upnpDiscover(timeoutMs, nullptr, nullptr, 0, 0, 2, &discoverError);
    if (!devlist) {
        qWarning() << "[UPNP] No UPnP devices found, error=" << discoverError;
        emit error(QString("UPnP discovery failed: no devices found (error=%1)").arg(discoverError));
        return false;
    }

    // Try to find a valid IGD (Internet Gateway Device)
    m_Urls = new UPNPUrls();
    m_Data = new IGDdatas();
    memset(m_LanAddr, 0, sizeof(m_LanAddr));

    char wanAddr[64] = {};
    int igdStatus = UPNP_GetValidIGD(devlist, m_Urls, m_Data,
                                     m_LanAddr, sizeof(m_LanAddr),
                                     wanAddr, sizeof(wanAddr));

    // Free the device list — we have extracted what we need
    freeUPNPDevlist(devlist);

    qInfo() << "[UPNP] IGD discovery status:" << igdStatus;

    if (igdStatus < 1) {
        // 0 = no IGD found
        // Negative = error during discovery
        qWarning() << "[UPNP] No valid IGD found, status=" << igdStatus;
        emit error(QString("No valid IGD gateway found (status=%1)").arg(igdStatus));
        cleanup();
        return false;
    }

    // Status 1 = valid IGD found (WANIPConnection or WANPPPConnection)
    // Status 2 = valid IGD but not connected to the internet
    // Status 3 = WANIPC device found but not a valid IGD
    if (igdStatus == 1) {
        qInfo() << "[UPNP] Valid IGD found and connected";
    } else if (igdStatus == 2) {
        qWarning() << "[UPNP] Valid IGD found but NOT connected to internet";
        // Still usable — we can add mappings that will work when it connects
    } else {
        qWarning() << "[UPNP] IGD found but not usable, status=" << igdStatus;
        cleanup();
        return false;
    }

    m_Available = true;
    m_GatewayAddr = QHostAddress(QString::fromLatin1(m_LanAddr));

    qInfo() << "[UPNP] IGD found: LAN addr=" << m_LanAddr
            << "controlURL=" << m_Urls->controlURL
            << "serviceType=" << m_Data->first.servicetype;
    return true;
#endif
}

bool UPNPClient::getExistingPortMapping(uint16_t externalPort,
                                            const std::string& protocol,
                                            std::string& internalClient,
                                            std::string& internalPort)
{
#ifndef MW_HAVE_MINIUPNPC
    Q_UNUSED(externalPort);
    Q_UNUSED(protocol);
    Q_UNUSED(internalClient);
    Q_UNUSED(internalPort);
    return false;
#else
    if (!m_Available || !m_Urls || !m_Data) {
        return false;
    }

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", externalPort);

    char intClient[64] = {};
    char intPort[8] = {};
    char desc[128] = {};
    char enabled[4] = {};
    char leaseDur[16] = {};

    int ret = UPNP_GetSpecificPortMappingEntry(
        m_Urls->controlURL,
        m_Data->first.servicetype,
        portStr,
        protocol.c_str(),
        nullptr,        // remoteHost
        intClient,
        intPort,
        desc,
        enabled,
        leaseDur);

    if (ret != 0) {
        // 714 = NoSuchEntryInArray (not an error, just means the port is free)
        if (ret != 714) {
            const char* errStr = strupnperror(ret);
            qInfo() << "[UPNP] GetSpecificPortMappingEntry for port" << externalPort
                    << protocol.c_str() << ":" << errStr << "(error=" << ret << ")";
        }
        return false;
    }

    internalClient = std::string(intClient);
    internalPort = std::string(intPort);

    qInfo() << "[UPNP] Existing port mapping found:" << externalPort
            << protocol.c_str() << "->" << intClient << ":" << intPort
            << "desc='" << desc << "' enabled='" << enabled
            << "' lease='" << leaseDur << "'";

    return true;
#endif
}

bool UPNPClient::addPortMapping(uint16_t externalPort, uint16_t internalPort,
                                 uint32_t leaseDurationSec, const std::string& desc,
                                 const std::string& protocol)
{
#ifndef MW_HAVE_MINIUPNPC
    Q_UNUSED(externalPort);
    Q_UNUSED(internalPort);
    Q_UNUSED(leaseDurationSec);
    Q_UNUSED(desc);
    qWarning() << "[UPNP] miniupnpc not available — cannot add port mapping";
    return false;
#else
    if (!m_Available || !m_Urls || !m_Data) {
        qWarning() << "[UPNP] Cannot add mapping: IGD not available";
        emit error("Cannot add port mapping: IGD not available");
        return false;
    }

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", externalPort);

    char internalPortStr[8];
    snprintf(internalPortStr, sizeof(internalPortStr), "%u", internalPort);

    // Use m_LanAddr directly (the interface that discovered the IGD).
    // Do NOT use getLocalIP() — it may return a VirtualBox/tunnel adapter
    // on multi-interface machines, causing the router to forward UDP to
    // the wrong host and breaking ICE.
    char intAddr[64] = {};
    strncpy(intAddr, m_LanAddr, sizeof(intAddr) - 1);

    qInfo() << "[UPNP] Adding port mapping:" << externalPort << protocol.c_str() << "->"
            << intAddr << ":" << internalPort
            << "(lease=" << leaseDurationSec << "s, desc=" << desc.c_str() << ")";

    int ret = UPNP_AddPortMapping(
        m_Urls->controlURL,
        m_Data->first.servicetype,
        portStr,                    // external port (string)
        internalPortStr,            // internal port (string)
        intAddr,                    // internal client address
        desc.c_str(),               // description
        protocol.c_str(),           // protocol
        nullptr,                    // remote host (null = any)
        leaseDurationSec > 0 ? std::to_string(leaseDurationSec).c_str() : nullptr);

    if (ret != 0) {
        const char* errStr = strupnperror(ret);
        qWarning() << "[UPNP] AddPortMapping failed:" << errStr << "(error=" << ret << ")";
        emit error(QString("AddPortMapping failed (port=%1): %2")
                   .arg(externalPort)
                   .arg(errStr));
        return false;
    }

    qInfo() << "[UPNP] Port mapping added successfully:" << externalPort << protocol.c_str();
    emit mappingAdded(externalPort);
    return true;
#endif
}

bool UPNPClient::removePortMapping(uint16_t externalPort, const std::string& protocol)
{
#ifndef MW_HAVE_MINIUPNPC
    Q_UNUSED(externalPort);
    Q_UNUSED(protocol);
    return false;
#else
    if (!m_Available || !m_Urls || !m_Data) {
        qWarning() << "[UPNP] Cannot remove mapping: IGD not available";
        return false;
    }

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", externalPort);

    qInfo() << "[UPNP] Removing port mapping:" << externalPort << protocol.c_str();

    int ret = UPNP_DeletePortMapping(
        m_Urls->controlURL,
        m_Data->first.servicetype,
        portStr,      // external port
        protocol.c_str(), // protocol
        nullptr);     // remote host (null = any)

    if (ret != 0) {
        const char* errStr = strupnperror(ret);
        // Non-fatal: the mapping may have already expired (lease timeout)
        qInfo() << "[UPNP] RemovePortMapping returned" << ret << ":" << errStr
                << "(may have already expired)";
        return false;
    }

    qInfo() << "[UPNP] Port mapping removed:" << externalPort << protocol.c_str();
    emit mappingRemoved(externalPort);
    return true;
#endif
}

std::string UPNPClient::getExternalIPAddress()
{
#ifndef MW_HAVE_MINIUPNPC
    return {};
#else
    if (!m_Available || !m_Urls || !m_Data) {
        qWarning() << "[UPNP] Cannot get external IP: IGD not available";
        return {};
    }

    char extAddr[64] = {};
    int ret = UPNP_GetExternalIPAddress(
        m_Urls->controlURL,
        m_Data->first.servicetype,
        extAddr);

    if (ret != 0) {
        const char* errStr = strupnperror(ret);
        qWarning() << "[UPNP] GetExternalIPAddress failed:" << errStr << "(error=" << ret << ")";
        return {};
    }

    qInfo() << "[UPNP] External IP address:" << extAddr;
    return std::string(extAddr);
#endif
}

void UPNPClient::cleanup()
{
#ifdef MW_HAVE_MINIUPNPC
    if (m_Urls) {
        FreeUPNPUrls(m_Urls);
        delete m_Urls;
        m_Urls = nullptr;
    }
    delete m_Data;
    m_Data = nullptr;
#endif
    m_Available = false;
    m_GatewayAddr.clear();
    memset(m_LanAddr, 0, sizeof(m_LanAddr));
}

bool UPNPClient::getLocalIP(char* buf, size_t bufsize)
{
#ifdef Q_OS_WIN
    // Use GetAdaptersAddresses to find the default interface IP
    DWORD buflen = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST
                         | GAA_FLAG_SKIP_MULTICAST
                         | GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &buflen);

    if (buflen == 0) return false;

    std::vector<unsigned char> bufVec(buflen);
    PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(bufVec.data());

    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST
                                      | GAA_FLAG_SKIP_MULTICAST
                                      | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, addresses, &buflen);

    if (ret != ERROR_SUCCESS) return false;

    for (PIP_ADAPTER_ADDRESSES aa = addresses; aa; aa = aa->Next) {
        // Skip loopback and tunnel adapters
        if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            aa->IfType == IF_TYPE_TUNNEL)
            continue;

        // Prefer Ethernet / Wi-Fi
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;

            sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            inet_ntop(AF_INET, &sa->sin_addr, buf, static_cast<DWORD>(bufsize));
            return true;
        }
    }

    // Fallback: gethostname + gethostbyname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent* he = gethostbyname(hostname);
        if (he && he->h_addr_list[0]) {
            inet_ntop(AF_INET, he->h_addr_list[0], buf, bufsize);
            return true;
        }
    }

    return false;
#else
    // Unix/macOS: getifaddrs
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1)
        return false;

    bool found = false;
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;

        struct sockaddr_in* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        inet_ntop(AF_INET, &sa->sin_addr, buf, bufsize);
        found = true;
        break;
    }

    freeifaddrs(ifaddr);
    return found;
#endif
}
