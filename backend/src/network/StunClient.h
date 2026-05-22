#pragma once

#include <QObject>
#include <QHostAddress>
#include <QString>
#include <QList>

/**
 * @brief Simple STUN client for public IPv4 address detection.
 *
 * Sends STUN binding requests (RFC 5389) to a configurable list of STUN servers
 * and parses the XOR-MAPPED-ADDRESS from the response to determine the
 * public IPv4 address as seen by the STUN server.
 *
 * Fallback chain: tries each server in order until one responds successfully.
 *
 * Thread safety: call from the Qt main thread only.
 */
class StunClient : public QObject
{
    Q_OBJECT

public:
    explicit StunClient(QObject* parent = nullptr);
    ~StunClient() override = default;

    /**
     * @brief A STUN server descriptor.
     */
    struct StunServer {
        QString host;   ///< Hostname or IP
        quint16 port;   ///< Port (typically 3478, 19302, 443, 80)
    };

    /// Default STUN server list (fallback chain).
    static QList<StunServer> defaultServers();

    /**
     * @brief Detect public IPv4 address by querying STUN servers.
     *
     * Tries each server in the given list in order until one succeeds.
     * Each server is given up to `timeoutMs` to respond.
     *
     * @param servers List of STUN servers to try.
     * @param timeoutMs Timeout per server in milliseconds.
     * @param[out] publicIp Set to the detected public IP on success.
     * @return true if a STUN server responded with our public IPv4 address.
     */
    bool detectPublicIp(const QList<StunServer>& servers,
                        int timeoutMs,
                        QString& publicIp);

signals:
    void error(const QString& message);

private:
    /// Send a STUN binding request to a single server and parse the response.
    /// Returns true if a valid XOR-MAPPED-ADDRESS was received.
    bool queryServer(const StunServer& server, int timeoutMs, QString& publicIp);

    /// Build a STUN binding request (20-byte header, no attributes).
    QByteArray buildBindingRequest();

    /// Parse a STUN response and extract XOR-MAPPED-ADDRESS (type 0x0020).
    /// Returns the parsed IPv4 address as a string, or empty on failure.
    QString parseXorMappedAddress(const QByteArray& response,
                                  const QByteArray& transactionId);
};
