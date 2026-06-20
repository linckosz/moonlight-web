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

#include <QString>
#include <QHostAddress>

#define MW_HTTP_PORT  47989
#define MW_HTTPS_PORT 47989

class NvAddress
{
public:
    NvAddress()
        : m_Port(0) {}

    explicit NvAddress(const QString& addr, quint16 port = MW_HTTP_PORT)
        : m_Address(addr), m_Port(port) {}

    explicit NvAddress(const QHostAddress& addr, quint16 port = MW_HTTP_PORT)
        : m_Address(addr.toString()), m_Port(port) {}

    QString address() const { return m_Address; }
    void setAddress(const QString& addr) { m_Address = addr; }
    void setAddress(const QHostAddress& addr) { m_Address = addr.toString(); }

    quint16 port() const { return m_Port; }
    void setPort(quint16 port) { m_Port = port; }

    bool isNull() const { return m_Address.isEmpty(); }

    QString toString() const
    {
        return m_Port ? (m_Address + ":" + QString::number(m_Port)) : m_Address;
    }

    bool operator==(const NvAddress& other) const
    {
        return m_Address == other.m_Address && m_Port == other.m_Port;
    }

    bool operator!=(const NvAddress& other) const
    {
        return !(*this == other);
    }

private:
    QString m_Address;
    quint16 m_Port;
};
