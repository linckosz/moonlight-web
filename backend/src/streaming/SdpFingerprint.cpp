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

#include "SdpFingerprint.h"

#include <QRegularExpression>
#include <QStringList>

#include <cctype>

namespace {

constexpr int SHA256_BYTES = 32;

/// Canonical form of a fingerprint value: uppercase hex, colon separated, or
/// empty when @p value is not exactly 32 hex bytes in that shape.
QString normalise(const QString& value)
{
    const QStringList bytes = value.split(u':', Qt::SkipEmptyParts);
    if (bytes.size() != SHA256_BYTES) return {};

    QString out;
    out.reserve(SHA256_BYTES * 3);
    for (const QString& b : bytes) {
        if (b.size() != 2) return {};
        for (const QChar c : b) {
            if (!isxdigit(c.toLatin1())) return {};
        }
        if (!out.isEmpty()) out.append(u':');
        out.append(b.toUpper());
    }
    return out;
}

} // namespace

namespace SdpFingerprint {

QString extract(const QString& sdp)
{
    QString found;

    // SDP lines are CRLF-terminated by the RFC but LF-only in practice; split on
    // both rather than trusting either.
    const QStringList lines =
        sdp.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);

    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (!line.startsWith(QStringLiteral("a=fingerprint:"), Qt::CaseInsensitive)) continue;

        const QString body = line.mid(int(sizeof("a=fingerprint:") - 1)).trimmed();
        const int sep = body.indexOf(u' ');
        if (sep <= 0) return {}; // "a=fingerprint:" with no algorithm/value split

        const QString algorithm = body.left(sep).trimmed();
        // Anything but sha-256 is a downgrade attempt or a peer we cannot bind
        // safely; either way we refuse rather than sign a weaker hash.
        if (algorithm.compare(QStringLiteral("sha-256"), Qt::CaseInsensitive) != 0) return {};

        const QString value = normalise(body.mid(sep + 1).trimmed());
        if (value.isEmpty()) return {};

        if (found.isEmpty()) {
            found = value;
        } else if (found != value) {
            // Two different fingerprints in one SDP: refuse. Picking either one
            // would mean signing a value the peer might not be the one using.
            return {};
        }
    }

    return found;
}

} // namespace SdpFingerprint
