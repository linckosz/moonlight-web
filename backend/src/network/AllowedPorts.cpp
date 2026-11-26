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

#include "AllowedPorts.h"

#include "RouterHoles.h"
#include "RouterPortPools.h"
#include "common/Edition.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace mw::routerports {

namespace {

AllowedPorts refused(const QString& why)
{
    AllowedPorts out;
    out.state = AllowedPorts::State::Invalid;
    out.why = why;
    return out;
}

QString rangeText(const PortRange& r)
{
    return r.first == r.last ? QString::number(r.first)
                             : QStringLiteral("%1-%2").arg(r.first).arg(r.last);
}

/// A whole number of 1 to 65535, or a string "a-b" of two such with a <= b.
bool itemOf(const QJsonValue& value, PortRange& out)
{
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (!std::isfinite(d) || d != std::floor(d) || d < 1 || d > 65535) return false;
        out.first = out.last = static_cast<uint16_t>(d);
        return true;
    }
    if (!value.isString()) return false;
    static const QRegularExpression kRange(QStringLiteral("^([0-9]{1,5})-([0-9]{1,5})$"));
    const QRegularExpressionMatch m = kRange.match(value.toString());
    if (!m.hasMatch()) return false;
    const int a = m.captured(1).toInt();
    const int b = m.captured(2).toInt();
    if (a < 1 || b > 65535 || a > b) return false;
    out.first = static_cast<uint16_t>(a);
    out.last = static_cast<uint16_t>(b);
    return true;
}

bool meets(const PortRange& a, uint16_t first, uint16_t last)
{
    return a.first <= last && first <= a.last;
}

// Set on the main thread, read on the router's too (RouterHoles' allowed()).
std::mutex g_Lock;
AllowedPorts g_Allowed;
QSet<uint16_t> g_Remembered;

} // namespace

bool AllowedPorts::contains(uint16_t port) const
{
    if (state != State::Valid) return false;
    for (const PortRange& r : ranges)
        if (port >= r.first && port <= r.last) return true;
    return false;
}

QList<uint16_t> AllowedPorts::ports() const
{
    QList<uint16_t> out;
    if (state != State::Valid) return out;
    for (const PortRange& r : ranges)
        for (int p = r.first; p <= r.last; ++p)
            out.append(static_cast<uint16_t>(p));
    return out;
}

QList<Forbidden> fixedForbidden()
{
    using namespace mw::edition;
    return {
        {1, 1023, QStringLiteral("below 1024")},
        {3478, 3481, QStringLiteral("the tunnel's STUN ports (3478-3481)")},
        {47984, 48010, QStringLiteral("the streaming servers' ports (47984-48010)")},
        {kDevMediaBasePort, static_cast<uint16_t>(kDevMediaBasePort + kMediaPortCount - 1),
         QStringLiteral("a DEV identity's stream block")},
        {kDevTunnelPoolBegin, kDevTunnelPoolEnd, QStringLiteral("a DEV identity's tunnel ports")},
        {kDevMediaPoolBegin, kDevMediaPoolEnd, QStringLiteral("a DEV identity's media pool")},
        {kDevHttpPort, kDevHttpPort, QStringLiteral("a DEV identity's HTTP port")},
        {kDevHttpsPort, kDevHttpsPort, QStringLiteral("a DEV identity's HTTPS port")},
        // Its signaling base, its relay and control ports, and its slots'
        // (server/SlotPortPlan.h: 48598-48645 for a DEV identity)
        {kDevSignalingPort, static_cast<uint16_t>(kDevSignalingPort + 2),
         QStringLiteral("a DEV identity's signaling ports")},
        {48598, 48645, QStringLiteral("a DEV identity's signaling ports")},
        {mw::routerholes::kKnownQuestionPort, mw::routerholes::kKnownQuestionPort,
         QStringLiteral("the port the router is asked about first, which nothing writes")},
    };
}

AllowedPorts parseAllowedPorts(const QJsonValue& stored, const QList<Forbidden>& local)
{
    const QString key = QLatin1String(kAllowedKey);
    if (stored.isUndefined()) return {};
    if (!stored.isArray())
        return refused(QStringLiteral("%1 must be a list of ports and ranges").arg(key));
    const QJsonArray items = stored.toArray();
    if (items.isEmpty()) return refused(QStringLiteral("%1 is an empty list").arg(key));
    if (items.size() > kAllowedItemsMax)
        return refused(QStringLiteral("%1 has %2 items, more than %3")
                           .arg(key)
                           .arg(items.size())
                           .arg(kAllowedItemsMax));

    AllowedPorts out;
    out.state = AllowedPorts::State::Valid;
    int total = 0;
    for (qsizetype i = 0; i < items.size(); ++i) {
        PortRange r;
        if (!itemOf(items.at(i), r))
            return refused(QStringLiteral("%1: item %2 is neither a port (1-65535) nor a range "
                                          "\"a-b\" with a <= b")
                               .arg(key)
                               .arg(i + 1));
        for (const PortRange& before : std::as_const(out.ranges))
            if (meets(before, r.first, r.last))
                return refused(QStringLiteral("%1: %2 overlaps %3, given before it")
                                   .arg(key, rangeText(r), rangeText(before)));
        total += r.last - r.first + 1;
        if (total > kAllowedPortsMax)
            return refused(
                QStringLiteral("%1 names more than %2 ports").arg(key).arg(kAllowedPortsMax));
        out.ranges.append(r);
    }
    QList<Forbidden> forbidden = fixedForbidden();
    forbidden += local;
    for (const PortRange& r : std::as_const(out.ranges))
        for (const Forbidden& f : std::as_const(forbidden))
            if (f.first != 0 && meets(r, f.first, f.last))
                return refused(QStringLiteral("%1: %2 touches %3").arg(key, rangeText(r), f.what));
    return out;
}

void setAllowedPorts(const AllowedPorts& allowed)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    // A list in force that is replaced (refused once the server's own ports
    // are known) was this run's to draw from: its numbers stay this host's to
    // take back
    for (const uint16_t p : g_Allowed.ports())
        g_Remembered.insert(p);
    g_Allowed = allowed;
}

AllowedPorts allowedPorts()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    return g_Allowed;
}

void setRememberedForRemoval(const QSet<uint16_t>& remembered)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    g_Remembered = remembered;
}

bool removableByList(uint16_t port)
{
    if (port < 1024) return false;
    std::lock_guard<std::mutex> lock(g_Lock);
    return g_Allowed.contains(port) || g_Remembered.contains(port);
}

void tellAdmin(QJsonObject& status, bool admin)
{
    const QLatin1String key(kAllowedRefusedKey);
    if (admin)
        status[key] = allowedPorts().state == AllowedPorts::State::Invalid;
    else
        status.remove(key);
}

} // namespace mw::routerports
