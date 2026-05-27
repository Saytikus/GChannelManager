#pragma once

#include <QByteArray>
#include <vector>

#include "GChannelManager_global.h"
#include "IMessageCodec.h"

// =====================================================================
//  ПРИМЕР кодека (НЕ часть контракта — замените своим протоколом).
//
//  Формат кадра (little-endian):
//    [magic u8 = 0xA5][type u8][corrId u32][len u32][payload ...]
// =====================================================================
class GCHANNELMANAGER_EXPORT SimpleFrameCodec final : public IMessageCodec
{
public:
    enum Type : quint8 {
        Request        = 1,
        Reply          = 2,
        KeepAliveReq   = 3,
        KeepAliveReply = 4,
        Data           = 5
    };

    [[nodiscard]] QByteArray encodeRequest(quint32 correlationId,
                                           const QByteArray &payload) override;
    [[nodiscard]] QByteArray encodeReply(quint32 correlationId,
                                         const QByteArray &payload) override;
    [[nodiscard]] QByteArray encodeData(const QByteArray &payload) override;
    [[nodiscard]] QByteArray encodeKeepAlive() override;
    [[nodiscard]] std::vector<DecodedMessage> feed(const QByteArray &bytes) override;
    void reset() override { m_buf.clear(); }

    // --- утилиты, переиспользуемые в т.ч. демо-узлом ---
    struct RawFrame { quint8 type = 0; quint32 corrId = 0; QByteArray payload; };
    [[nodiscard]] static QByteArray makeFrame(quint8 type, quint32 corrId,
                                              const QByteArray &payload);
    [[nodiscard]] static std::vector<RawFrame> parse(QByteArray &buffer);   // потребляет разобранное из buffer

private:
    QByteArray m_buf;
};
