/*
 * MoonlightWeb. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#pragma once

#include <QList>
#include <QPair>
#include <QString>

// Announces this server on the LAN as `_moonlightweb._tcp` on its HTTPS port,
// so a native client (the Android TV app) lists it without an address typed.
//
// Through the operating system's own mDNS responder, never a socket of ours:
// holding UDP 5353 for the whole run steals unicast mDNS answers from Moonlight
// Qt on the same PC (see ComputerManager's discovery bursts). Windows 10 1809+
// has DnsServiceRegister; elsewhere start() only logs that nothing is announced
// (Avahi on Linux and dns_sd on macOS are the follow-ups).
class MdnsAdvertiser
{
public:
    MdnsAdvertiser() = default;
    ~MdnsAdvertiser();
    MdnsAdvertiser(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;

    static constexpr const char* kServiceType = "_moonlightweb._tcp";

    // The DNS-SD instance name for this machine: its host name, " DEV" for a
    // --dev instance (both can run on one PC), dots out (a dot would split the
    // label), at most 63 bytes of UTF-8 (one DNS label).
    static QString instanceName(const QString& hostName, bool dev);

    // The TXT record announced with the service, keys and values in the order
    // they go out: `v` "1", `id` @p hostId, and `name` @p name, at most 63 bytes
    // of UTF-8 cut between two characters, without spaces at either end. `name`
    // is left out when nothing is left of it.
    static QList<QPair<QString, QString>> txtRecord(const QString& hostId, const QString& name);

    // Starts announcing; false when this platform cannot (nothing to undo).
    // `hostId` goes in a TXT record `id`: the host key's name, what a pairing
    // returns as host_id, so that a client paired already recognises this machine
    // under another address or from its code. A claim, not a proof: it opens nothing.
    // `name` goes in a TXT record `name`: what this install calls itself
    // (AppSettings::displayName), for a client to list it under that name.
    bool start(const QString& instance, quint16 httpsPort, const QString& hostId,
               const QString& name);
    void stop();

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};
