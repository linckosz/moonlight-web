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

#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

/// Whether the machine at an address holds the certificate a host was paired
/// with. Asked before an answer from a new address moves a paired host: a host
/// whose lease changed passes, whatever else answers there does not.
namespace HostCertCheck {

/// The same certificate, whichever way its PEM was written. Nothing matches an
/// empty or unreadable one.
bool sameCertificate(const QByteArray& expectedPem, const QByteArray& candidatePem);

/// What a machine showed when asked for its certificate.
enum class Shown
{
    Expected, ///< the certificate on record
    Another,  ///< a certificate, not that one
    Nothing   ///< refused, silent or not TLS
};

/// Shakes hands with address:httpsPort, presenting the client identity as every
/// HTTPS call to a host does, and reports once what it showed, within timeoutMs.
/// Nothing is reported once context is gone.
void look(const QString& address, quint16 httpsPort, const QByteArray& expectedPem,
          const QByteArray& clientCertPem, const QByteArray& clientKeyPem, QObject* context,
          int timeoutMs, std::function<void(Shown)> done);

/// The same, for a caller to whom only the expected certificate is an answer.
void verify(const QString& address, quint16 httpsPort, const QByteArray& expectedPem,
            const QByteArray& clientCertPem, const QByteArray& clientKeyPem, QObject* context,
            int timeoutMs, std::function<void(bool)> done);

} // namespace HostCertCheck
