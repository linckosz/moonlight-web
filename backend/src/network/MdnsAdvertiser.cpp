/*
 * MoonlightWeb. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "MdnsAdvertiser.h"

#include <QDebug>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windns.h>
#ifdef _MSC_VER
#pragma comment(lib, "dnsapi.lib")
#endif
#endif

QString MdnsAdvertiser::instanceName(const QString& hostName, bool dev)
{
    QString name = hostName.trimmed();
    // A host name may come fully qualified: its first label is the machine.
    name = name.section(QLatin1Char('.'), 0, 0);
    if (name.isEmpty()) name = QStringLiteral("MoonlightWeb");
    const QString suffix = dev ? QStringLiteral(" DEV") : QString();
    // One DNS label: 63 bytes of UTF-8, the suffix kept whole.
    while (!name.isEmpty() && (name + suffix).toUtf8().size() > 63)
        name.chop(1);
    return name + suffix;
}

#ifdef Q_OS_WIN

struct MdnsAdvertiser::Impl
{
    std::wstring instance;
    std::wstring host;
    PDNS_SERVICE_INSTANCE service = nullptr;
    DNS_SERVICE_REGISTER_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
};

static VOID WINAPI onRegistered(DWORD status, PVOID, PDNS_SERVICE_INSTANCE instance)
{
    if (status != ERROR_SUCCESS) qWarning() << "mDNS: announcement failed, status" << status;
    if (instance) DnsServiceFreeInstance(instance);
}

bool MdnsAdvertiser::start(const QString& instance, quint16 httpsPort, const QString& hostId)
{
    stop();
    if (httpsPort == 0) return false;
    auto* impl = new Impl;
    impl->instance =
        (instance + QLatin1Char('.') + QLatin1String(kServiceType) + QStringLiteral(".local"))
            .toStdWString();
    // Windows' responder already answers for <computer name>.local.
    impl->host =
        (QSysInfo::machineHostName().section(QLatin1Char('.'), 0, 0) + QStringLiteral(".local"))
            .toStdWString();
    const std::wstring id = hostId.toStdWString();
    PCWSTR keys[] = {L"v", L"id"};
    PCWSTR values[] = {L"1", id.c_str()};
    impl->service = DnsServiceConstructInstance(impl->instance.c_str(), impl->host.c_str(), nullptr,
                                                nullptr, httpsPort, 0, 0, 2, keys, values);
    if (!impl->service) {
        qWarning() << "mDNS: cannot build the service instance";
        delete impl;
        return false;
    }
    impl->request.Version = DNS_QUERY_REQUEST_VERSION1;
    impl->request.InterfaceIndex = 0; // every interface
    impl->request.pServiceInstance = impl->service;
    impl->request.pRegisterCompletionCallback = onRegistered;
    impl->request.unicastEnabled = FALSE;
    const DWORD status = DnsServiceRegister(&impl->request, &impl->cancel);
    if (status != DNS_REQUEST_PENDING) {
        qWarning() << "mDNS: DnsServiceRegister refused, status" << status;
        DnsServiceFreeInstance(impl->service);
        delete impl;
        return false;
    }
    m_Impl = impl;
    qInfo().noquote() << "mDNS: announcing" << instance << "as" << kServiceType << "on port"
                      << httpsPort;
    return true;
}

void MdnsAdvertiser::stop()
{
    if (!m_Impl) return;
    // The goodbye packet: clients drop the server at once instead of at TTL expiry.
    DnsServiceDeRegister(&m_Impl->request, nullptr);
    DnsServiceFreeInstance(m_Impl->service);
    delete m_Impl;
    m_Impl = nullptr;
}

#else

struct MdnsAdvertiser::Impl
{};

bool MdnsAdvertiser::start(const QString& instance, quint16, const QString&)
{
    qInfo().noquote() << "mDNS: no announcement of" << instance << "on this platform yet";
    return false;
}

void MdnsAdvertiser::stop() {}

#endif

MdnsAdvertiser::~MdnsAdvertiser()
{
    stop();
}
