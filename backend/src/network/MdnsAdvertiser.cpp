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
#elif defined(MW_HAVE_QTDBUS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QVariant>
#elif defined(Q_OS_MACOS)
#include <arpa/inet.h>
#include <dns_sd.h>
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

QList<QByteArray> MdnsAdvertiser::txtStrings(const QString& hostId, const QString& name)
{
    QList<QByteArray> strings;
    for (const QPair<QString, QString>& entry : txtRecord(hostId, name))
        strings.append(entry.first.toUtf8() + '=' + entry.second.toUtf8());
    return strings;
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

#elif defined(MW_HAVE_QTDBUS)

// Avahi, the responder of nearly every Linux desktop and server, through its
// D-Bus interface on the system bus: an entry group holds the service, and
// freeing the group withdraws it. Each call is bounded, so a bus that never
// answers costs the start a few seconds at most, never the server.

namespace {

constexpr int kAvahiTimeoutMs = 2000;
// AVAHI_IF_UNSPEC and AVAHI_PROTO_UNSPEC: every interface, IPv4 and IPv6.
constexpr int kAvahiEveryInterface = -1;
constexpr int kAvahiEveryProtocol = -1;

QDBusMessage avahiCall(const QString& path, const QString& interface, const QString& method,
                       const QList<QVariant>& arguments = {})
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Avahi"),
                                                       path, interface, method);
    call.setArguments(arguments);
    return QDBusConnection::systemBus().call(call, QDBus::Block, kAvahiTimeoutMs);
}

QDBusMessage groupCall(const QString& group, const QString& method,
                       const QList<QVariant>& arguments = {})
{
    return avahiCall(group, QStringLiteral("org.freedesktop.Avahi.EntryGroup"), method, arguments);
}

bool failed(const QDBusMessage& reply, const char* what)
{
    if (reply.type() == QDBusMessage::ReplyMessage) return false;
    qWarning().noquote() << "mDNS:" << what << "failed:" << reply.errorName()
                         << reply.errorMessage();
    return true;
}

} // namespace

struct MdnsAdvertiser::Impl
{
    QString group; // the entry group's object path
};

bool MdnsAdvertiser::start(const QString& instance, quint16 httpsPort, const QString& hostId,
                           const QString& name)
{
    stop();
    if (httpsPort == 0) return false;
    if (!QDBusConnection::systemBus().isConnected()) {
        // A container, or a system without D-Bus: nothing to announce through.
        qInfo().noquote() << "mDNS: no system bus, no announcement of" << instance;
        return false;
    }
    const QDBusMessage created =
        avahiCall(QStringLiteral("/"), QStringLiteral("org.freedesktop.Avahi.Server"),
                  QStringLiteral("EntryGroupNew"));
    if (failed(created, "reaching Avahi")) return false;
    const QString group = qvariant_cast<QDBusObjectPath>(created.arguments().value(0)).path();
    if (group.isEmpty()) {
        qWarning() << "mDNS: Avahi gave no entry group";
        return false;
    }
    // The TXT strings go out as an array of byte arrays (aay).
    qDBusRegisterMetaType<QList<QByteArray>>();
    // An empty domain and host: Avahi's own, local and this machine's name.
    const QList<QVariant> service{QVariant::fromValue(kAvahiEveryInterface),
                                  QVariant::fromValue(kAvahiEveryProtocol),
                                  QVariant::fromValue(0u), // flags
                                  instance,
                                  QString::fromLatin1(kServiceType),
                                  QString(),
                                  QString(),
                                  QVariant::fromValue(httpsPort),
                                  QVariant::fromValue(txtStrings(hostId, name))};
    if (failed(groupCall(group, QStringLiteral("AddService"), service), "adding the service") ||
        failed(groupCall(group, QStringLiteral("Commit")), "committing the service")) {
        groupCall(group, QStringLiteral("Free"));
        return false;
    }
    m_Impl = new Impl{group};
    qInfo().noquote() << "mDNS: announcing" << instance << "as" << kServiceType << "on port"
                      << httpsPort << "through Avahi";
    return true;
}

void MdnsAdvertiser::stop()
{
    if (!m_Impl) return;
    // Avahi withdraws the records with a goodbye packet: clients drop the
    // server at once. It does the same when this process leaves the bus.
    groupCall(m_Impl->group, QStringLiteral("Free"));
    delete m_Impl;
    m_Impl = nullptr;
}

#elif defined(Q_OS_MACOS)

// mDNSResponder, through dns_sd (part of the system). No reply is read: the
// responder renames the instance itself on a conflict, and its reply would only
// say under which name; an error it can see at once, DNSServiceRegister returns.

struct MdnsAdvertiser::Impl
{
    DNSServiceRef ref = nullptr;
};

bool MdnsAdvertiser::start(const QString& instance, quint16 httpsPort, const QString& hostId,
                           const QString& name)
{
    stop();
    if (httpsPort == 0) return false;
    TXTRecordRef txt;
    TXTRecordCreate(&txt, 0, nullptr);
    for (const QPair<QString, QString>& entry : txtRecord(hostId, name)) {
        const QByteArray key = entry.first.toUtf8();
        const QByteArray value = entry.second.toUtf8();
        // One TXT string is at most 255 bytes, its key and '=' included.
        if (key.size() + 1 + value.size() > 255 ||
            TXTRecordSetValue(&txt, key.constData(), static_cast<uint8_t>(value.size()),
                              value.constData()) != kDNSServiceErr_NoError) {
            qWarning() << "mDNS: cannot build the TXT record";
            TXTRecordDeallocate(&txt);
            return false;
        }
    }
    DNSServiceRef ref = nullptr;
    const QByteArray instanceUtf8 = instance.toUtf8();
    const DNSServiceErrorType error =
        DNSServiceRegister(&ref, 0, kDNSServiceInterfaceIndexAny, instanceUtf8.constData(),
                           kServiceType, nullptr, nullptr, htons(httpsPort),
                           TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr);
    TXTRecordDeallocate(&txt);
    if (error != kDNSServiceErr_NoError) {
        qWarning() << "mDNS: DNSServiceRegister refused, error" << error;
        return false;
    }
    m_Impl = new Impl{ref};
    qInfo().noquote() << "mDNS: announcing" << instance << "as" << kServiceType << "on port"
                      << httpsPort;
    return true;
}

void MdnsAdvertiser::stop()
{
    if (!m_Impl) return;
    // Closing the connection withdraws the records, with a goodbye packet.
    DNSServiceRefDeallocate(m_Impl->ref);
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
