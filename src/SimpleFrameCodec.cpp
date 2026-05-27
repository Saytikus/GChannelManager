#include "GChannelManager/SimpleFrameCodec.h"

namespace {
constexpr quint8 kMagic  = 0xA5;
constexpr qint32 kHeader = 1 + 1 + 4 + 4;   // magic + type + corrId + len

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
} // namespace

QByteArray SimpleFrameCodec::makeFrame(quint8 type, quint32 corrId, const QByteArray &payload)
{
    QByteArray b;
    b.reserve(kHeader + payload.size());
    b.append(char(kMagic));
    b.append(char(type));
    putU32(b, corrId);
    putU32(b, quint32(payload.size()));
    b.append(payload);
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
    return makeFrame(Data, 0, payload);   // корреляция не нужна
}

QByteArray SimpleFrameCodec::encodeKeepAlive()
{
    return makeFrame(KeepAliveReq, 0, {});
}

std::vector<SimpleFrameCodec::RawFrame> SimpleFrameCodec::parse(QByteArray &buf)
{
    std::vector<RawFrame> out;
    qint32 pos = 0;
    while (buf.size() - pos >= kHeader) {
        const char *h = buf.constData() + pos;
        if (quint8(h[0]) != kMagic) { ++pos; continue; }   // ресинхронизация по magic

        const quint32 len = getU32(h + 6);
        if (buf.size() - pos < kHeader + qint32(len))
            break;                                          // кадр пришёл не целиком

        RawFrame f;
        f.type    = quint8(h[1]);
        f.corrId  = getU32(h + 2);
        f.payload = buf.mid(pos + kHeader, qint32(len));
        out.push_back(std::move(f));
        pos += kHeader + qint32(len);
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
        case Request:        m.type = DecodedMessage::Type::Request;   break;
        case Reply:          m.type = DecodedMessage::Type::Reply;     break;
        case KeepAliveReply: m.type = DecodedMessage::Type::KeepAlive; break;
        case Data:           m.type = DecodedMessage::Type::Data;      break;
        default:             m.type = DecodedMessage::Type::Unknown;   break;   // KeepAliveReq и пр.
        }
        out.push_back(std::move(m));
    }
    return out;
}
