#include "StunClient.h"

#include <QUdpSocket>
#include <QUrl>
#include <QHostInfo>
#include <QDateTime>
#include <QRandomGenerator>
#include <QDebug>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

// ---------------------------------------------------------------------------
// STUN protocol constants (RFC 5389)
// ---------------------------------------------------------------------------

static constexpr quint16 kStunBindingRequest   = 0x0001;
static constexpr quint16 kStunBindingResponse  = 0x0101;
static constexpr quint32 kStunMagicCookie      = 0x2112A442;

// Attribute types
static constexpr quint16 kAttrXorMappedAddress = 0x0020;
static constexpr quint16 kAttrMappedAddress    = 0x0001;

// Header size: 20 bytes
static constexpr int kStunHeaderSize = 20;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StunClient::StunClient(QObject* parent)
    : QObject(parent)
{
}

// ---------------------------------------------------------------------------
// Default server list
// ---------------------------------------------------------------------------

QList<StunClient::StunServer> StunClient::defaultServers()
{
    return {
        { QStringLiteral("stun.l.google.com"),       19302 },
        { QStringLiteral("stun1.l.google.com"),      19302 },
        { QStringLiteral("stun.cloudflare.com"),     3478  },
        { QStringLiteral("stun.nextcloud.com"),      443   },
        { QStringLiteral("relay.metered.ca"),        80    },
    };
}

// ---------------------------------------------------------------------------
// Main detection method (fallback chain)
// ---------------------------------------------------------------------------

bool StunClient::detectPublicIp(const QList<StunServer>& servers,
                                int timeoutMs,
                                QString& publicIp)
{
    for (const auto& server : servers) {
        qInfo() << "[StunClient] Trying STUN server:" << server.host << ":" << server.port;

        if (queryServer(server, timeoutMs, publicIp)) {
            qInfo() << "[StunClient] Detected public IP:" << publicIp
                    << "from" << server.host << ":" << server.port;
            return true;
        }

        qWarning() << "[StunClient] STUN server failed:" << server.host << ":" << server.port;
    }

    qWarning() << "[StunClient] All STUN servers failed";
    emit error(QStringLiteral("All STUN servers failed to respond"));
    return false;
}

// ---------------------------------------------------------------------------
// Query a single STUN server
// ---------------------------------------------------------------------------

bool StunClient::queryServer(const StunServer& server, int timeoutMs, QString& publicIp)
{
    QUdpSocket socket;

    // Build binding request
    QByteArray request = buildBindingRequest();

    // Extract transaction ID from request (bytes 8-19)
    QByteArray transactionId = request.mid(8, 12);

    // Resolve hostname
    QHostAddress addr;
    if (!addr.setAddress(server.host)) {
        // Hostname — do a blocking DNS lookup
        QHostInfo info = QHostInfo::fromName(server.host);
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            qWarning() << "[StunClient] DNS resolution failed for"
                       << server.host << ":" << info.errorString();
            return false;
        }
        // Prefer IPv4 so the STUN server returns an IPv4 XOR-MAPPED-ADDRESS
        const auto& addrs = info.addresses();
        QHostAddress v6Fallback;
        bool found = false;
        for (const auto& a : addrs) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                addr = a;
                found = true;
                break;
            }
            if (v6Fallback.isNull() && a.protocol() == QAbstractSocket::IPv6Protocol)
                v6Fallback = a;
        }
        if (!found) {
            if (!v6Fallback.isNull()) {
                addr = v6Fallback;
                qDebug() << "[StunClient] No IPv4 for" << server.host
                         << ", falling back to IPv6";
            } else {
                qWarning() << "[StunClient] No usable address for" << server.host;
                return false;
            }
        }
    }

    // Connect and send
    socket.connectToHost(addr, server.port);
    if (!socket.waitForConnected(timeoutMs)) {
        qWarning() << "[StunClient] Connection timeout after"
                   << timeoutMs << "ms to" << server.host << ":" << server.port;
        return false;
    }

    qint64 sent = socket.write(request);
    if (sent != request.size()) {
        qWarning() << "[StunClient] Failed to send STUN request to"
                   << server.host << ":" << server.port
                   << "— wrote" << sent << "of" << request.size() << "bytes";
        return false;
    }

    if (!socket.flush()) {
        // flush may not be supported on all platforms; try waitForBytesWritten
        socket.waitForBytesWritten(timeoutMs);
    }

    // Wait for response
    if (!socket.waitForReadyRead(timeoutMs)) {
        qWarning() << "[StunClient] Read timeout waiting for STUN response from"
                   << server.host << ":" << server.port
                   << "(timeout:" << timeoutMs << "ms)";
        socket.disconnectFromHost();
        return false;
    }

    QByteArray response = socket.readAll();
    socket.disconnectFromHost();

    if (response.isEmpty()) {
        qWarning() << "[StunClient] Empty response from"
                   << server.host << ":" << server.port;
        return false;
    }

    // Parse the XOR-MAPPED-ADDRESS from response
    publicIp = parseXorMappedAddress(response, transactionId);
    return !publicIp.isEmpty();
}

// ---------------------------------------------------------------------------
// Build STUN binding request
// ---------------------------------------------------------------------------

QByteArray StunClient::buildBindingRequest()
{
    QByteArray data;
    data.resize(kStunHeaderSize);

    // Type: Binding Request (0x0001) — network byte order
    data[0] = static_cast<char>((kStunBindingRequest >> 8) & 0xFF);
    data[1] = static_cast<char>(kStunBindingRequest & 0xFF);

    // Length: 0 (no attributes) — network byte order
    data[2] = 0;
    data[3] = 0;

    // Magic Cookie — network byte order
    data[4] = static_cast<char>((kStunMagicCookie >> 24) & 0xFF);
    data[5] = static_cast<char>((kStunMagicCookie >> 16) & 0xFF);
    data[6] = static_cast<char>((kStunMagicCookie >> 8) & 0xFF);
    data[7] = static_cast<char>(kStunMagicCookie & 0xFF);

    // Transaction ID: 12 random bytes
    for (int i = 0; i < 12; ++i) {
        data[8 + i] = static_cast<char>(
            QRandomGenerator::global()->bounded(256));
    }

    return data;
}

// ---------------------------------------------------------------------------
// Parse XOR-MAPPED-ADDRESS from STUN response
// ---------------------------------------------------------------------------

QString StunClient::parseXorMappedAddress(const QByteArray& response,
                                          const QByteArray& transactionId)
{
    if (response.size() < kStunHeaderSize) {
        qWarning() << "[StunClient] Response too short:" << response.size();
        return {};
    }

    QString ipv6Fallback;

    // Validate response type: should be Binding Response (0x0101)
    quint16 type = (static_cast<quint8>(response[0]) << 8)
                 | static_cast<quint8>(response[1]);
    if (type != kStunBindingResponse) {
        qWarning() << "[StunClient] Unexpected STUN response type:" << type;
        return {};
    }

    // Validate magic cookie
    quint32 cookie = (static_cast<quint8>(response[4]) << 24)
                   | (static_cast<quint8>(response[5]) << 16)
                   | (static_cast<quint8>(response[6]) << 8)
                   | static_cast<quint8>(response[7]);
    if (cookie != kStunMagicCookie) {
        // Legacy (RFC 3489) response — not supported
        qWarning() << "[StunClient] Invalid magic cookie in STUN response";
        return {};
    }

    // Extract transaction ID from response
    QByteArray respTid = response.mid(8, 12);
    if (respTid != transactionId) {
        qWarning() << "[StunClient] Transaction ID mismatch in STUN response";
        return {};
    }

    // Parse attributes after the header
    int messageLength = (static_cast<quint8>(response[2]) << 8)
                      | static_cast<quint8>(response[3]);
    int pos = kStunHeaderSize;
    int end = pos + messageLength;

    while (pos + 4 <= end && pos + 4 <= response.size()) {
        quint16 attrType = (static_cast<quint8>(response[pos]) << 8)
                         | static_cast<quint8>(response[pos + 1]);
        quint16 attrLength = (static_cast<quint8>(response[pos + 2]) << 8)
                           | static_cast<quint8>(response[pos + 3]);

        pos += 4;

        // Clamp attrLength to remaining data
        int avail = response.size() - pos;
        if (attrLength > avail)
            attrLength = static_cast<quint16>(avail);

        if (attrType == kAttrXorMappedAddress && attrLength >= 8) {
            // XOR-MAPPED-ADDRESS:
            //   1 byte: 0 padding
            //   1 byte: family (0x01=IPv4, 0x02=IPv6)
            //   2 bytes: port (XORed with magic cookie first 2 bytes)
            //   4 bytes: IPv4 address (XORed with magic cookie)
            //   or 16 bytes: IPv6 address (XORed with transaction ID)
            quint8 family = static_cast<quint8>(response[pos + 1]);

            if (family == 0x01) {
                // IPv4
                // XOR port with magic cookie's high 16 bits (0x2112)
                quint16 xorPort = (static_cast<quint8>(response[pos + 2]) << 8)
                                | static_cast<quint8>(response[pos + 3]);
                quint16 port = xorPort ^ (kStunMagicCookie >> 16);

                Q_UNUSED(port);

                // XOR address with magic cookie (32 bits)
                quint32 xorAddr = (static_cast<quint8>(response[pos + 4]) << 24)
                                | (static_cast<quint8>(response[pos + 5]) << 16)
                                | (static_cast<quint8>(response[pos + 6]) << 8)
                                | static_cast<quint8>(response[pos + 7]);
                quint32 addr = xorAddr ^ kStunMagicCookie;

                // Construct IPv4 address from raw bytes (works across Qt 5/6)
                QHostAddress ha;
                ha.setAddress(QStringLiteral("%1.%2.%3.%4")
                    .arg((addr >> 24) & 0xFF)
                    .arg((addr >> 16) & 0xFF)
                    .arg((addr >> 8) & 0xFF)
                    .arg(addr & 0xFF));
                return ha.toString();
            } else if (family == 0x02 && attrLength >= 20) {
                // IPv6 — save as fallback in case no IPv4 is found
                quint32 xorWords[4];
                for (int i = 0; i < 4; ++i) {
                    xorWords[i] = (static_cast<quint8>(response[pos + 4 + i*4]) << 24)
                                | (static_cast<quint8>(response[pos + 5 + i*4]) << 16)
                                | (static_cast<quint8>(response[pos + 6 + i*4]) << 8)
                                | static_cast<quint8>(response[pos + 7 + i*4]);
                }
                quint32 words[4];
                words[0] = xorWords[0] ^ kStunMagicCookie;
                for (int i = 1; i < 4; ++i) {
                    quint32 tidPart = (static_cast<quint8>(transactionId[(i-1)*4]) << 24)
                                    | (static_cast<quint8>(transactionId[(i-1)*4+1]) << 16)
                                    | (static_cast<quint8>(transactionId[(i-1)*4+2]) << 8)
                                    | static_cast<quint8>(transactionId[(i-1)*4+3]);
                    words[i] = xorWords[i] ^ tidPart;
                }
                QHostAddress ha;
                ha.setAddress(QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8")
                    .arg((words[0] >> 16) & 0xFFFF, 4, 16, QChar('0'))
                    .arg(words[0] & 0xFFFF, 4, 16, QChar('0'))
                    .arg((words[1] >> 16) & 0xFFFF, 4, 16, QChar('0'))
                    .arg(words[1] & 0xFFFF, 4, 16, QChar('0'))
                    .arg((words[2] >> 16) & 0xFFFF, 4, 16, QChar('0'))
                    .arg(words[2] & 0xFFFF, 4, 16, QChar('0'))
                    .arg((words[3] >> 16) & 0xFFFF, 4, 16, QChar('0'))
                    .arg(words[3] & 0xFFFF, 4, 16, QChar('0')));
                ipv6Fallback = ha.toString();
            }
        } else if (attrType == kAttrMappedAddress && attrLength >= 8) {
            // Non-XOR MAPPED-ADDRESS (RFC 3489 fallback)
            // Some servers may still send this
            quint8 family = static_cast<quint8>(response[pos + 1]);
            if (family == 0x01) {
                quint32 addr = (static_cast<quint8>(response[pos + 4]) << 24)
                             | (static_cast<quint8>(response[pos + 5]) << 16)
                             | (static_cast<quint8>(response[pos + 6]) << 8)
                             | static_cast<quint8>(response[pos + 7]);
                QHostAddress ha;
                ha.setAddress(QStringLiteral("%1.%2.%3.%4")
                    .arg((addr >> 24) & 0xFF)
                    .arg((addr >> 16) & 0xFF)
                    .arg((addr >> 8) & 0xFF)
                    .arg(addr & 0xFF));
                return ha.toString();
            }
        }

        // Advance to next attribute (padded to 4-byte boundary)
        pos += attrLength;
        if (pos % 4 != 0) {
            pos += 4 - (pos % 4);
        }
    }

    if (!ipv6Fallback.isEmpty()) {
        qDebug() << "[StunClient] No IPv4 XOR-MAPPED-ADDRESS, using IPv6 fallback:"
                 << ipv6Fallback;
        return ipv6Fallback;
    }
    qWarning() << "[StunClient] No XOR-MAPPED-ADDRESS found in STUN response";
    return {};
}
