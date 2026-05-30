#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <vector>

#include "DecodedMessage.h"

// =====================================================================
//  The protocol-level codec contract.
//
//  The gateway itself does not know your protocol's frame format. The codec:
//    * packs a request into a frame, embedding the correlationId (to match
//      a reply to its request during "send and await a reply");
//    * builds a keep-alive frame;
//    * parses the incoming byte stream into messages and classifies them.
//
//  feed() must buffer incomplete frames between calls.
// =====================================================================
class IMessageCodec {
public:
    virtual ~IMessageCodec() = default;

    // Pack a request into a frame.
    [[nodiscard]] virtual QByteArray encodeRequest(quint32 correlationId,
                                                   const QByteArray &payload) = 0;

    // Pack a reply to an incoming request — correlation matches the request.
    // Used by Gateway::reply().
    [[nodiscard]] virtual QByteArray encodeReply(quint32 correlationId,
                                                 const QByteArray &payload) = 0;

    // Pack "uncorrelated" data (fire-and-forget): no correlation needed,
    // no reply expected. Used by Gateway::send().
    [[nodiscard]] virtual QByteArray encodeData(const QByteArray &payload) = 0;

    // Session-lifecycle frames — a channel separate from keep-alive.
    [[nodiscard]] virtual QByteArray encodeSessionStart()    = 0;
    [[nodiscard]] virtual QByteArray encodeSessionStartAck() = 0;
    [[nodiscard]] virtual QByteArray encodeSessionStop()     = 0;

    // Build a keep-alive request frame (the heartbeat we initiate).
    [[nodiscard]] virtual QByteArray encodeKeepAlive() = 0;

    // Build a keep-alive reply frame — the answer to a peer's keep-alive
    // request. The Gateway sends it automatically on a KeepAlivePing so two
    // instances of this library can keep each other alive.
    [[nodiscard]] virtual QByteArray encodeKeepAliveReply() = 0;

    // Feed raw bytes, get back a list of decoded messages.
    [[nodiscard]] virtual std::vector<DecodedMessage> feed(const QByteArray &bytes) = 0;

    // Reset the internal buffer (e.g. on a link drop / session restart).
    virtual void reset() {}
};
