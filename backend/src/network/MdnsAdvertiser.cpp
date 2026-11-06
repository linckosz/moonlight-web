/*
 * MoonlightWeb. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "MdnsAdvertiser.h"

#include <QByteArray>
#include <QDebug>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windns.h>
#ifdef _MSC_VER
#pragma comment(lib, "dnsapi.lib")
#endif
#include <string>
#include <vector>
#endif

namespace {

// One DNS label's worth: the longest name a TXT record carries.
constexpr qsizetype kTxtNameMaxBytes = 63;

} // namespace

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

QList<QPair<QString, QString>> MdnsAdvertiser::txtRecord(const QString& hostId, const QString& name)
{
    QList<QPair<QString, QString>> txt{qMakePair(QStringLiteral("v"), QStringLiteral("1")),
                                       qMakePair(QStringLiteral("id"), hostId)};
    // A client reads the value as UTF-8, so the cut falls between two
    // characters: back from the limit over the continuation bytes (10xxxxxx)
    // of the character it would split.
    QByteArray utf8 = name.trimmed().toUtf8();
    if (utf8.size() > kTxtNameMaxBytes) {
        qsizetype cut = kTxtNameMaxBytes;
        while (cut > 0 && (static_cast<uchar>(utf8.at(cut)) & 0xC0) == 0x80)
            --cut;
        utf8.truncate(cut);
    }
    const QString kept = QString::fromUtf8(utf8).trimmed();
    if (!kept.isEmpty()) txt.append(qMakePair(QStringLiteral("name"), kept));
    return txt;
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

bool MdnsAdvertiser::start(const QString& instance, quint16 httpsPort, const QString& hostId,
                           const QString& name)
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
    // The TXT strings, held here while the instance below copies them.
    const QList<QPair<QString, QString>> txt = txtRecord(hostId, name);
    std::vector<std::wstring> text;
    for (const QPair<QString, QString>& entry : txt) {
        text.push_back(entry.first.toStdWString());
        text.push_back(entry.second.toStdWString());
    }
    std::vector<PCWSTR> keys;
    std::vector<PCWSTR> values;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        keys.push_back(text[i].c_str());
        values.push_back(text[i + 1].c_str());
    }
    impl->service = DnsServiceConstructInstance(
        impl->instance.c_str(), impl->host.c_str(), nullptr, nullptr, httpsPort, 0, 0,
        static_cast<DWORD>(keys.size()), keys.data(), values.data());
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

bool MdnsAdvertiser::start(const QString& instance, quint16, const QString&, const QString&)
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
