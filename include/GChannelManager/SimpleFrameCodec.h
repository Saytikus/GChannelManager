#pragma once

#include <QByteArray>
#include <vector>

#include "GChannelManager_global.h"
#include "IMessageCodec.h"

// =====================================================================
//  EXAMPLE codec (NOT part of the contract — replace with your protocol).
//
//  Frame format (little-endian):
//    [magic u8 = 0xA5][type u8][corrId u32][len u32][payload ...][crc16 u16]
//
//  The trailing CRC-16/CCITT-FALSE covers everything from `magic` through the
//  end of `payload`; a frame that fails the check is dropped and the parser
//  resynchronizes. `len` is bounded by kMaxPayloadSize so a corrupt/hostile
//  header can never drive an out-of-bounds read or unbounded buffering.
// =====================================================================
class GCHANNELMANAGER_EXPORT SimpleFrameCodec final : public IMessageCodec
{
public:
    enum Type : quint8 {
        Request         = 1,
        Reply           = 2,
        KeepAliveReq    = 3,
        KeepAliveReply  = 4,
        Data            = 5,
        SessionStart    = 6,
        SessionStartAck = 7,
        SessionStop     = 8
    };

    [[nodiscard]] QByteArray encodeRequest(quint32 correlationId,
                                           const QByteArray &payload) override;
    [[nodiscard]] QByteArray encodeReply(quint32 correlationId,
                                         const QByteArray &payload) override;
    [[nodiscard]] QByteArray encodeData(const QByteArray &payload) override;
    [[nodiscard]] QByteArray encodeSessionStart() override;
    [[nodiscard]] QByteArray encodeSessionStartAck() override;
    [[nodiscard]] QByteArray encodeSessionStop() override;
    [[nodiscard]] QByteArray encodeKeepAlive() override;
    [[nodiscard]] QByteArray encodeKeepAliveReply() override;
    [[nodiscard]] std::vector<DecodedMessage> feed(const QByteArray &bytes) override;
    void reset() override { m_buf.clear(); }

    // Largest payload this codec will accept in a single frame. A header that
    // claims more is treated as line noise and skipped (resync), bounding both
    // memory use and the parse window.
    static constexpr quint32 kMaxPayloadSize = 16u * 1024u * 1024u;   // 16 MiB

    // --- utilities, reused e.g. by the demo peer ---
    struct RawFrame { quint8 type = 0; quint32 corrId = 0; QByteArray payload; };
    [[nodiscard]] static QByteArray makeFrame(quint8 type, quint32 corrId,
                                              const QByteArray &payload);
    [[nodiscard]] static std::vector<RawFrame> parse(QByteArray &buffer);   // consumes what it parses from buffer

private:
    QByteArray m_buf;
};
