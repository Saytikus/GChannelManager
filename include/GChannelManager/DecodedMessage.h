#pragma once

#include <QByteArray>
#include <QtGlobal>

// =====================================================================
//  The result of decoding a single message from the codec.
//  `type` determines how the `payload` field (and `correlationId` for
//  Reply) is to be interpreted by the calling code.
// =====================================================================
struct DecodedMessage {
    enum class Type {
        Reply,            // reply to our request -> look at correlationId
        Request,          // request from the peer to us -> app answers via Gateway::reply()
        SessionStart,     // peer initiates a session (we must answer with SessionStartAck)
        SessionStartAck,  // peer acknowledged our SessionStart (we move to Active)
        SessionStop,      // peer ends the session
        KeepAlive,        // keep-alive (link liveness confirmation)
        Data,             // data without correlation (push from the peer)
        Unknown           // unrecognized / service
    };

    Type       type          = Type::Unknown;
    quint32    correlationId = 0;   // valid for Reply and Request
    QByteArray payload;
};
