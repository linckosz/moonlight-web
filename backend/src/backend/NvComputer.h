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

#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>
#include <QSettings>

#include "NvAddress.h"
#include "NvApp.h"
#include "NvHTTP.h"

class NvComputer
{
public:
    enum PairState
    {
        PS_UNKNOWN,
        PS_PAIRED,
        PS_NOT_PAIRED
    };
    enum ComputerState
    {
        CS_UNKNOWN,
        CS_ONLINE,
        CS_OFFLINE
    };

    NvComputer() = default;

    // Construct from serverInfo XML + active address (polling response)
    explicit NvComputer(const QString& serverInfo, const NvAddress& activeAddr);

    // Construct from persisted QSettings
    explicit NvComputer(QSettings& settings);

    // Merge polling data — returns true if any field changed
    bool update(const NvComputer& that);

    // Persistence
    void serialize(QSettings& settings) const;
    bool isEqualSerialized(const NvComputer& that) const;

    // REST API output
    QJsonObject toJson() const;

    // Helpers for address iteration during polling
    QVector<NvAddress> uniqueAddresses() const;

    // True when this host resolves to the machine MoonlightWeb runs on (loopback
    // or one of our own interface addresses). Surfaced in toJson() so the
    // frontend can warn about streaming your own PC ("Inception" loop).
    bool isLocalMachine() const;

    // State management
    static PairState pairStateFromString(const QString& s);
    static QString pairStateToString(PairState ps);
    static ComputerState computerStateFromString(const QString& s);
    static QString computerStateToString(ComputerState cs);

    // — Ephemeral traits (from polling) —
    ComputerState state = CS_UNKNOWN;

    // Consecutive failed polls — debounce so a single transient network
    // error (stale keep-alive, 2s abort) does not flip the host offline.
    int consecutivePollFailures = 0;

    // True when the machine answers at the IP level but the GameStream/Sunshine
    // server isn't (e.g. the host is powered on but MoonlightWeb/Sunshine isn't
    // running → the serverinfo poll gets a TCP reset, not a timeout). Lets the
    // frontend show "Unavailable" (no Wake-on-LAN) instead of a plain "Offline"
    // + Wake for a host that is demonstrably up. Ephemeral, not persisted.
    bool reachable = false;

    // — Persisted pairing state —
    PairState pairState = PS_UNKNOWN;
    NvAddress activeAddress;
    int currentGameId = 0;
    QString gfeVersion;
    QString appVersion;
    QString gpuModel;
    int serverCodecModeSupport = 0;
    int maxLumaPixelsHEVC = 0;
    bool isNvidiaServerSoftware = false;
    QVector<NvDisplayMode> displayModes;

    // — Persisted traits —
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress manualAddress;
    QByteArray macAddress;
    QString name;
    QString uuid;
    QVector<NvApp> appList;

    // — Pairing state (persisted) —
    QByteArray serverCertPem;
    quint16 activeHttpsPort = 0;

    // — Stream backend (persisted) —
    //
    // Empty means a plain GameStream host: Sunshine, Apollo, anything speaking
    // NvHTTP. That is the default and keeps those hosts on exactly today's code
    // path. A non-empty type names a registered backend ("wolf") with its own
    // control API, addressed through backendApiUrl.
    //
    // backendApiToken is a secret and must never reach toJson().
    QString backendType;
    QString backendApiUrl;
    QString backendApiToken;

    // Credentials used to push a pairing PIN to whatever the backend puts in
    // front of each seat. MultiSeat needs them: its own API can read and unpair
    // clients but never pair one, so a seat is paired through its Apollo web UI
    // — and MultiSeat does not generate those credentials, it expects an admin
    // to have set them. All seats share one credentials file, so one pair
    // covers every seat. Empty for backends that pair without them, like Wolf.
    //
    // backendPairPassword is a secret and must never reach toJson().
    QString backendPairUser;
    QString backendPairPassword;

    // — What we worked out about this host ourselves (NOT persisted) —
    //
    // A MultiSeat control API answered on this host's address. This is the one
    // thing about a backend that can be established without a credential and
    // without asking, so it is what lets the UI offer MultiSeat to the people
    // who have it while staying completely absent for everyone else.
    //
    // Deliberately not saved: it describes what is running right now, and a
    // stale "yes" from months ago would offer a setup that cannot complete.
    // Re-probed once per process, per host.
    bool multiSeatApiPresent = false;
};
