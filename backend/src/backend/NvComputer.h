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
    enum PairState    { PS_UNKNOWN, PS_PAIRED, PS_NOT_PAIRED };
    enum ComputerState { CS_UNKNOWN, CS_ONLINE, CS_OFFLINE };

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

    // State management
    static PairState pairStateFromString(const QString& s);
    static QString pairStateToString(PairState ps);
    static ComputerState computerStateFromString(const QString& s);
    static QString computerStateToString(ComputerState cs);

    // — Ephemeral traits (from polling) —
    ComputerState state = CS_UNKNOWN;
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
};
