#pragma once

#include <QByteArray>
#include <QJsonObject>

// Encodes JSON input events into GameStream wire-format binary packets.
// Each packet includes the 4-byte BE length prefix required by the TCP
// control channel (port 35043).
class InputEncoder
{
public:
    // Returns the encoded packet, or empty QByteArray for unknown event types.
    static QByteArray encodeFromJson(const QJsonObject& msg);

private:
    static QByteArray encodeKeyEvent(const QJsonObject& msg, bool down);
    static QByteArray encodeMouseMove(const QJsonObject& msg);
    static QByteArray encodeMouseButton(const QJsonObject& msg, bool down);
    static QByteArray encodeMouseScroll(const QJsonObject& msg);
};
