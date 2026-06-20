/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#pragma once

#include <QObject>
#include <QHostAddress>
#include <cstdint>
#include <string>
#include <memory>

struct UPNPUrls;
struct IGDdatas;

// UPnP-IGD port mapping client using miniupnpc.
// Discovers the Internet Gateway Device on the LAN and manages UDP port mappings
// for WebRTC NAT traversal.
//
// Thread safety: all methods should be called from the Qt main thread only.
class UPNPClient : public QObject
{
    Q_OBJECT

public:
    explicit UPNPClient(QObject* parent = nullptr);
    ~UPNPClient() override;

    // Discover UPnP IGD gateway on the LAN.
    // timeoutMs: max time (ms) to wait for M-SEARCH responses.
    // Returns true if a valid IGD was found and is connected.
    bool discover(int timeoutMs = 2000);

    // Add a port mapping from externalPort to this host's internalPort.
    // leaseDurationSec: 0 = permanent, 3600 = 1 hour (recommended for routers
    // that do not support permanent mappings).
    // desc: human-readable description shown in the router admin UI.
    // protocol: "TCP" or "UDP" (default: "UDP" for backward compat).
    // Returns true if the mapping was added successfully.
    bool addPortMapping(uint16_t externalPort,
                        uint16_t internalPort,
                        uint32_t leaseDurationSec = 0,
                        const std::string& desc = "Moonlight-Web",
                        const std::string& protocol = "UDP");

    // Check if a port mapping already exists on the IGD.
    // Returns true if a mapping exists; internalClient and internalPort are set.
    // Returns false if no mapping exists or the IGD is not available.
    bool getExistingPortMapping(uint16_t externalPort,
                                const std::string& protocol,
                                std::string& internalClient,
                                std::string& internalPort);

    // Remove a port mapping. protocol must match the original mapping.
    bool removePortMapping(uint16_t externalPort,
                           const std::string& protocol = "UDP");

    // Get the public IP address as seen by the IGD.
    // Returns empty string on failure or if no IGD is available.
    std::string getExternalIPAddress();

    // Get the local IP address of the IGD gateway.
    QHostAddress gatewayAddress() const { return m_GatewayAddr; }

    // Get the LAN IP address of this host on the IGD-facing interface.
    std::string lanAddress() const { return std::string(m_LanAddr); }

    // Check if a valid IGD was discovered and is available.
    bool isAvailable() const { return m_Available; }

    // Get the local LAN IP address of this host by inspecting network interfaces.
    // Returns true on success; buf is filled with a dotted-decimal IPv4 string.
    static bool getLocalIP(char* buf, size_t bufsize);

signals:
    void mappingAdded(uint16_t port);
    void mappingRemoved(uint16_t port);
    void error(const QString& message);

private:
    void cleanup();

    bool m_Available = false;
    QHostAddress m_GatewayAddr;

    // miniupnpc state
    UPNPUrls* m_Urls = nullptr;
    IGDdatas* m_Data = nullptr;
    char m_LanAddr[64] = {};
};
