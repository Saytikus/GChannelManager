#include "GChannelManager/SimpleFrameCodec.h"

namespace {
constexpr quint8    kMagic  = 0xA5;
constexpr qsizetype kHeader = 1 + 1 + 4 + 4;   // magic + type + corrId + len
constexpr qsizetype kCrc    = 2;               // trailing CRC-16

void putU32(QByteArray &b, quint32 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8)  & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}
quint32 getU32(const char *p) {
    return  quint32(quint8(p[0]))
         | (quint32(quint8(p[1])) << 8)
         | (quint32(quint8(p[2])) << 16)
         | (quint32(quint8(p[3])) << 24);
}
void putU16(QByteArray &b, quint16 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}
quint16 getU16(const char *p) {
    return quint16(quint8(p[0])) | quint16(quint16(quint8(p[1])) << 8);
}

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection). A small
// bitwise implementation — fine for an example codec.
quint16 crc16(const char *data, qsizetype len) {
    quint16 crc = 0xFFFF;
    for (qsizetype i = 0; i < len; ++i) {
        crc ^= quint16(quint16(quint8(data[i])) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021)
                                 : quint16(crc << 1);
    }
    return crc;
}
} // namespace

QByteArray SimpleFrameCodec::makeFrame(quint8 type, quint32 corrId, const QByteArray &payload)
{
    QByteArray b;
    b.reserve(kHeader + payload.size() + kCrc);
    b.append(char(kMagic));
    b.append(char(type));
    putU32(b, corrId);
    putU32(b, quint32(payload.size()));
    b.append(payload);
    putU16(b, crc16(b.constData(), b.size()));   // CRC over magic..payload
    return b;
}

QByteArray SimpleFrameCodec::encodeRequest(quint32 correlationId, const QByteArray &payload)
{
    return makeFrame(Request, correlationId, payload);
}

QByteArray SimpleFrameCodec::encodeReply(quint32 correlationId, const QByteArray &payload)
{
    return makeFrame(Reply, correlationId, payload);
}

QByteArray SimpleFrameCodec::encodeData(const QByteArray &payload)
{
    return makeFrame(Data, 0, payload);   // no correlation needed
}

QByteArray SimpleFrameCodec::encodeSessionStart()
{
    return makeFrame(SessionStart, 0, {});
}

QByteArray SimpleFrameCodec::encodeSessionStartAck()
{
    return makeFrame(SessionStartAck, 0, {});
}

QByteArray SimpleFrameCodec::encodeSessionStop()
{
    return makeFrame(SessionStop, 0, {});
}

QByteArray SimpleFrameCodec::encodeKeepAlive()
{
    return makeFrame(KeepAliveReq, 0, {});
}

QByteArray SimpleFrameCodec::encodeKeepAliveReply()
{
    return makeFrame(KeepAliveReply, 0, {});
}

std::vector<SimpleFrameCodec::RawFrame> SimpleFrameCodec::parse(QByteArray &buf)
{
    std::vector<RawFrame> out;
    qsizetype pos = 0;
    while (buf.size() - pos >= kHeader) {
        const char *h = buf.constData() + pos;
        if (quint8(h[0]) != kMagic) { ++pos; continue; }   // resync by magic

        const quint32 len = getU32(h + 6);
        // A length beyond the cap is line noise, not a real frame: skip this
        // magic byte and resync. (Guards against OOB reads and unbounded
        // buffering from a corrupt/hostile header — never trust `len`.)
        if (len > kMaxPayloadSize) { ++pos; continue; }

        // Compare in 64-bit; `len` is now safely bounded.
        const qsizetype need = kHeader + qsizetype(len) + kCrc;
        if (buf.size() - pos < need)
            break;                                          // frame not fully arrived yet

        // Verify the trailing CRC before trusting the frame; on mismatch the
        // magic was spurious — skip it and resync.
        const quint16 want = getU16(h + kHeader + qsizetype(len));
        if (crc16(h, kHeader + qsizetype(len)) != want) { ++pos; continue; }

        RawFrame f;
        f.type    = quint8(h[1]);
        f.corrId  = getU32(h + 2);
        f.payload = buf.mid(pos + kHeader, qsizetype(len));
        out.push_back(std::move(f));
        pos += need;
    }
    if (pos > 0)
        buf.remove(0, pos);
    return out;
}

std::vector<DecodedMessage> SimpleFrameCodec::feed(const QByteArray &bytes)
{
    m_buf.append(bytes);
    std::vector<DecodedMessage> out;
    for (const auto &f : parse(m_buf)) {
        DecodedMessage m;
        m.correlationId = f.corrId;
        m.payload       = f.payload;
        switch (f.type) {
        case Request:         m.type = DecodedMessage::Type::Request;         break;
        case Reply:           m.type = DecodedMessage::Type::Reply;           break;
        case KeepAliveReq:    m.type = DecodedMessage::Type::KeepAlivePing;   break;
        case KeepAliveReply:  m.type = DecodedMessage::Type::KeepAlive;       break;
        case Data:            m.type = DecodedMessage::Type::Data;            break;
        case SessionStart:    m.type = DecodedMessage::Type::SessionStart;    break;
        case SessionStartAck: m.type = DecodedMessage::Type::SessionStartAck; break;
        case SessionStop:     m.type = DecodedMessage::Type::SessionStop;     break;
        default:              m.type = DecodedMessage::Type::Unknown;         break;
        }
        out.push_back(std::move(m));
    }
    return out;
}
