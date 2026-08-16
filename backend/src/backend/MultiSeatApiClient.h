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

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/// One entry of GET /api/seats (MultiSeat.Shared/Models/SeatInfo.cs).
///
/// A seat is a provisioned Windows account plus its own Apollo instance, so
/// `portBase` is what makes it reachable: MultiSeat mirrors Apollo's
/// map_port(N) = base + N, and MoonlightWeb only needs the two GameStream
/// entry points out of that block.
struct MultiSeatSeat
{
    QString id;          ///< guid
    QString accountName; ///< the Windows account backing this seat
    int sessionId = -1;  ///< Windows session id, -1 when not logged in
    QString status;      ///< Idle|Provisioning|Configuring|Ready|Streaming|TearingDown|Error
    int width = 0;
    int height = 0;
    int fps = 0;
    int portBase = 0;
    int apolloProcessId = 0;

    /// Why a seat is in Error, straight from the service. Worth surfacing
    /// rather than reducing to "unusable": MultiSeat writes genuinely
    /// actionable text here (which prerequisite to re-run, which registry value
    /// to check), and a seat that failed to provision is the single most likely
    /// thing an admin will be staring at.
    QString errorMessage;

    /// How far provisioning got — Session, Display, Audio, Apollo… Read with
    /// errorMessage, it says which step failed.
    QString provisioningStep;

    /// Apollo's GFE ports for this seat, from Constants.cs. Offsets are -5 and
    /// 0; the rest of the block (video, control, audio, mic, RTSP) is Apollo's
    /// own business and never dialled from here.
    quint16 gfeHttpsPort() const
    {
        return portBase > 0 ? static_cast<quint16>(portBase - 5) : 0;
    }
    quint16 gfeHttpPort() const { return portBase > 0 ? static_cast<quint16>(portBase) : 0; }

    /// Only a Ready or Streaming seat has a usable Apollo behind it.
    bool isUsable() const
    {
        return portBase > 0
               && (status == QStringLiteral("Ready") || status == QStringLiteral("Streaming"));
    }
};

/// Why a MultiSeat API call failed. Transport-level on purpose: MultiSeatBackend
/// maps these onto BackendError so user-facing wording stays in one place.
struct MultiSeatApiError
{
    enum Kind
    {
        None,
        Unreachable, ///< never got an answer
        Timeout,
        Unauthorized, ///< 401/403 — wrong or missing X-MultiSeat-Key
        HttpError,    ///< answered, but not 2xx
        Protocol,     ///< 2xx whose body did not parse
    };

    Kind kind = None;
    int httpStatus = 0;
    QString message;

    static MultiSeatApiError make(Kind k, const QString& msg, int status = 0)
    {
        return MultiSeatApiError{k, status, msg};
    }
};

using MultiSeatVoidCallback = std::function<void(bool ok, const MultiSeatApiError& error)>;
using MultiSeatSeatsCallback = std::function<void(bool ok, const MultiSeatApiError& error,
                                                  const QVector<MultiSeatSeat>& seats)>;
using MultiSeatSeatCallback =
    std::function<void(bool ok, const MultiSeatApiError& error, const MultiSeatSeat& seat)>;
using MultiSeatJsonArrayCallback =
    std::function<void(bool ok, const MultiSeatApiError& error, const QJsonArray& items)>;

/**
 * @brief Client for MultiSeat's REST API (`http://<host>:9550/api`).
 *
 * MultiSeat turns one Windows box into several independent GameStream hosts:
 * each seat is a Windows account with a virtual display, its own audio device
 * and its own Apollo instance. That last part is what matters here — a seat is
 * reachable as an ordinary GameStream host at `<host>:<portBase>`, so pairing
 * and streaming reuse NvHTTP untouched. This client only does the part NvHTTP
 * cannot: enumerating and provisioning the seats.
 *
 * Auth is the `X-MultiSeat-Key` header (Constants.cs), whose value MultiSeat
 * auto-generates into `C:\ProgramData\MultiSeat\api-key.txt` unless configured.
 *
 * The service answers camelCase JSON with string enums — both are set
 * explicitly in ApiServer.cs, so `status` is "Ready", not an integer.
 *
 * Deliberately not sharing a base class with WolfApiClient: the two speak
 * different dialects (Wolf wraps everything in a {success,error} envelope and
 * lives behind a Unix socket, MultiSeat answers plain objects over TCP with a
 * key header). Two clients is not yet a pattern worth abstracting.
 */
class MultiSeatApiClient : public QObject
{
    Q_OBJECT

public:
    /// @param baseUrl Root of the service, e.g. "http://192.168.1.9:9550".
    /// @param apiKey  Sent as `X-MultiSeat-Key`. Empty is valid only when the
    ///                service was configured with `"ApiKey":"disabled"`.
    MultiSeatApiClient(QString baseUrl, QString apiKey, QNetworkAccessManager* nam,
                       QObject* parent = nullptr);

    QString baseUrl() const { return m_BaseUrl; }

    /// GET /api/seats
    void listSeats(MultiSeatSeatsCallback cb);

    /// GET /api/seats/{id}
    void getSeat(const QString& seatId, MultiSeatSeatCallback cb);

    /// POST /api/seats — provisions an account's seat (display, audio, Apollo).
    /// `params` is passed through as the SeatRequest body rather than mirrored
    /// into a struct: it grows upstream (NvencPreset, LaunchApp, …) and a typed
    /// copy here would silently drop whatever it does not know about.
    void provisionSeat(const QJsonObject& params, MultiSeatSeatCallback cb);

    /// DELETE /api/seats/{id}
    void teardownSeat(const QString& seatId, MultiSeatVoidCallback cb);

    /// GET /api/seats/{id}/clients — GameStream clients paired to that seat.
    void seatClients(const QString& seatId, MultiSeatJsonArrayCallback cb);

    /// DELETE /api/seats/{id}/clients — unpair every client of a seat.
    void clearSeatClients(const QString& seatId, MultiSeatVoidCallback cb);

    /// GET /api/accounts — raw, for the admin surface only.
    void listAccounts(MultiSeatJsonArrayCallback cb);

    /// Parse one SeatInfo object. Exposed for the backend, which also reads
    /// seats out of provisioning replies.
    static MultiSeatSeat parseSeat(const QJsonObject& obj);

private:
    using RawCallback = std::function<void(bool, const MultiSeatApiError&, const QJsonDocument&)>;

    void get(const QString& path, RawCallback cb);
    void post(const QString& path, const QJsonObject& body, RawCallback cb);
    void del(const QString& path, RawCallback cb);

    /// Shared reply handling: transport error, HTTP status, JSON parse.
    void finish(QNetworkReply* reply, RawCallback cb);

    QString m_BaseUrl;
    QString m_ApiKey;
    QNetworkAccessManager* m_Nam = nullptr;
};
