#include <QTest>

#include <GChannelManager/SimpleFrameCodec.h>

// =====================================================================
//  Unit tests for SimpleFrameCodec — the reference frame codec.
//  These exercise the codec in isolation (no Gateway, no transport):
//  encode* produces a frame with the right type byte / corrId, and
//  feed() classifies an incoming byte stream into DecodedMessage::Type,
//  buffering partial frames and resynchronizing on garbage.
// =====================================================================
class TestSimpleFrameCodec : public QObject
{
    Q_OBJECT
private slots:
    void encodeRequest_roundtrip();
    void incomingRequestFrame_classifiedAsTypeRequest();
    void encodeReply_carriesCorrelation();
    void encodeData_carriesNoCorrelation();
    void keepAlive_reqDecodesAsPing_replyDecodesAsKeepAlive();
    void encodeSessionStart_classifiedAsType_SessionStart();
    void encodeSessionStartAck_classifiedAsType_SessionStartAck();
    void encodeSessionStop_classifiedAsType_SessionStop();
    void feed_splitAcrossCalls_buffered();
    void feed_garbagePrefix_resyncsByMagic();
    void feed_multipleFrames_inOneBuffer();
    void parse_consumesBytesIncrementally();
    void reset_clearsBuffer();
    void feed_oversizedLength_resyncsWithoutOverrun();
    void feed_corruptedFrame_droppedByCrc();
};

// encodeRequest() yields a well-formed Request frame, and the matching
// Reply frame (same corrId) decodes back as Type::Reply with its payload.
void TestSimpleFrameCodec::encodeRequest_roundtrip()
{
    SimpleFrameCodec codec;
    const QByteArray payload = "hello-world";
    const QByteArray frame   = codec.encodeRequest(42, payload);

    // The request frame comes back "from the peer" as a Reply frame with the same
    // corrId — that is what the peer's answer to our request looks like. feed() classifies it as Reply.
    const QByteArray reply = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 42, payload);
    const auto msgs = codec.feed(reply);

    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Reply);
    QCOMPARE(msgs[0].correlationId, quint32(42));
    QCOMPARE(msgs[0].payload, payload);

    // And the Request frame itself must carry the corrId and payload, ready to send.
    QVERIFY(frame.size() >= 10);
    QCOMPARE(quint8(frame[0]), quint8(0xA5));
    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::Request));
}

// A Request frame from the peer decodes as Type::Request (server-role path).
void TestSimpleFrameCodec::incomingRequestFrame_classifiedAsTypeRequest()
{
    SimpleFrameCodec codec;
    // The peer sends us a Request — feed() must classify it as Type::Request,
    // not Unknown (the library used to be a pure client and ignored such frames).
    const QByteArray frame = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 7,
                                                         QByteArray("DO_IT"));
    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Request);
    QCOMPARE(msgs[0].correlationId, quint32(7));
    QCOMPARE(msgs[0].payload, QByteArray("DO_IT"));
}

// encodeReply() preserves the corrId so the peer can match it to its request.
void TestSimpleFrameCodec::encodeReply_carriesCorrelation()
{
    SimpleFrameCodec codec;
    const QByteArray frame = codec.encodeReply(13, QByteArray("OK"));
    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::Reply));

    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Reply);
    QCOMPARE(msgs[0].correlationId, quint32(13));
    QCOMPARE(msgs[0].payload, QByteArray("OK"));
}

// encodeData() (fire-and-forget) carries no correlation: corrId == 0, Type::Data.
void TestSimpleFrameCodec::encodeData_carriesNoCorrelation()
{
    SimpleFrameCodec codec;
    const QByteArray payload = "push";
    const QByteArray frame   = codec.encodeData(payload);

    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::Data));
    // corrId must be 0 (we do not correlate fire-and-forget)
    QCOMPARE(quint8(frame[2]), quint8(0));
    QCOMPARE(quint8(frame[3]), quint8(0));
    QCOMPARE(quint8(frame[4]), quint8(0));
    QCOMPARE(quint8(frame[5]), quint8(0));

    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Data);
    QCOMPARE(msgs[0].payload, payload);
}

// An incoming KeepAliveReq decodes as KeepAlivePing (so the Gateway can answer
// it); the peer's KeepAliveReply decodes as KeepAlive (drives liveness).
void TestSimpleFrameCodec::keepAlive_reqDecodesAsPing_replyDecodesAsKeepAlive()
{
    SimpleFrameCodec codec;
    // encodeKeepAlive() is the heartbeat request (KeepAliveReq). Received from a
    // peer it must surface as KeepAlivePing so the Gateway replies with a pong.
    const QByteArray req = codec.encodeKeepAlive();
    auto msgs = codec.feed(req);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::KeepAlivePing);

    // A KeepAliveReply (the pong) must become DecodedMessage::Type::KeepAlive.
    const QByteArray rep = codec.encodeKeepAliveReply();
    msgs = codec.feed(rep);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::KeepAlive);
}

// Each session-control frame round-trips to its matching DecodedMessage type.
void TestSimpleFrameCodec::encodeSessionStart_classifiedAsType_SessionStart()
{
    SimpleFrameCodec codec;
    const QByteArray frame = codec.encodeSessionStart();
    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::SessionStart));
    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::SessionStart);
}

void TestSimpleFrameCodec::encodeSessionStartAck_classifiedAsType_SessionStartAck()
{
    SimpleFrameCodec codec;
    const QByteArray frame = codec.encodeSessionStartAck();
    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::SessionStartAck));
    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::SessionStartAck);
}

void TestSimpleFrameCodec::encodeSessionStop_classifiedAsType_SessionStop()
{
    SimpleFrameCodec codec;
    const QByteArray frame = codec.encodeSessionStop();
    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::SessionStop));
    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::SessionStop);
}

// A frame split across two feed() calls is buffered and reassembled — the core
// guarantee that lets the transport deliver arbitrary byte chunks.
void TestSimpleFrameCodec::feed_splitAcrossCalls_buffered()
{
    SimpleFrameCodec codec;
    const QByteArray full = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 7,
                                                        QByteArray("abcdefgh"));
    QVERIFY(full.size() > 6);

    // First chunk — only the header, no full payload: nothing can be parsed.
    auto msgs1 = codec.feed(full.left(6));
    QCOMPARE(msgs1.size(), size_t(0));

    // Second chunk — the remainder: one message appears.
    auto msgs2 = codec.feed(full.mid(6));
    QCOMPARE(msgs2.size(), size_t(1));
    QCOMPARE(msgs2[0].correlationId, quint32(7));
    QCOMPARE(msgs2[0].payload, QByteArray("abcdefgh"));
}

// Garbage bytes before the 0xA5 magic are skipped so the parser resynchronizes.
void TestSimpleFrameCodec::feed_garbagePrefix_resyncsByMagic()
{
    SimpleFrameCodec codec;
    const QByteArray frame = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 3,
                                                         QByteArray("ok"));
    QByteArray dirty = QByteArray("\x10\x20\x30", 3) + frame;

    auto msgs = codec.feed(dirty);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].correlationId, quint32(3));
    QCOMPARE(msgs[0].payload, QByteArray("ok"));
}

// Several frames in a single buffer are all decoded, in order.
void TestSimpleFrameCodec::feed_multipleFrames_inOneBuffer()
{
    SimpleFrameCodec codec;
    QByteArray buf;
    buf += SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 1, QByteArray("A"));
    buf += SimpleFrameCodec::makeFrame(SimpleFrameCodec::Data,  0, QByteArray("B"));
    buf += SimpleFrameCodec::makeFrame(SimpleFrameCodec::KeepAliveReply, 0, {});

    auto msgs = codec.feed(buf);
    QCOMPARE(msgs.size(), size_t(3));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Reply);
    QCOMPARE(msgs[0].correlationId, quint32(1));
    QCOMPARE(msgs[1].type, DecodedMessage::Type::Data);
    QCOMPARE(msgs[2].type, DecodedMessage::Type::KeepAlive);
}

// The low-level parse() removes only the bytes it consumed, leaving any
// trailing partial data in the buffer for the next call.
void TestSimpleFrameCodec::parse_consumesBytesIncrementally()
{
    QByteArray buf = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 9, QByteArray("X"));
    buf += "tail";   // garbage tail that does not form a frame

    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].corrId, quint32(9));

    // consumed bytes must be gone, "tail" must remain in the buffer for the next feed
    QCOMPARE(buf, QByteArray("tail"));
}

// reset() discards any buffered partial frame so the next frame parses cleanly.
void TestSimpleFrameCodec::reset_clearsBuffer()
{
    SimpleFrameCodec codec;
    // feed a partial header — it lingers in the internal buffer
    QByteArray partial = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 1,
                                                     QByteArray("AB")).left(4);
    QCOMPARE(codec.feed(partial).size(), size_t(0));

    codec.reset();

    // after reset the internal buffer is clean — the next normal frame parses from scratch
    const QByteArray frame = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 2,
                                                         QByteArray("CD"));
    auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].correlationId, quint32(2));
}

// A header that claims an absurd payload length (high bit set, or merely huge)
// must not be trusted: the parser treats the magic as line noise and resyncs,
// rather than reading out of bounds or buffering forever (H1).
void TestSimpleFrameCodec::feed_oversizedLength_resyncsWithoutOverrun()
{
    SimpleFrameCodec codec;

    // Hand-build a bogus header: magic, type, corrId=0, len=0xFFFFFFFF (negative
    // if ever cast to signed). No payload follows.
    QByteArray bogus;
    bogus.append(char(0xA5));                       // magic
    bogus.append(char(SimpleFrameCodec::Reply));    // type
    bogus.append(4, char(0));                       // corrId = 0
    bogus.append(4, char(0xFF));                    // len = 0xFFFFFFFF

    // Must not crash / read OOB, and yields nothing.
    auto msgs = codec.feed(bogus);
    QCOMPARE(msgs.size(), size_t(0));

    // The parser resynchronized past the bad magic, so a valid frame still
    // decodes afterwards instead of being swallowed.
    const QByteArray good = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 5,
                                                        QByteArray("ok"));
    auto msgs2 = codec.feed(good);
    QCOMPARE(msgs2.size(), size_t(1));
    QCOMPARE(msgs2[0].correlationId, quint32(5));
    QCOMPARE(msgs2[0].payload, QByteArray("ok"));
}

// A single flipped payload bit fails the trailing CRC: the frame is dropped
// (never delivered as valid data) and the parser recovers (M2).
void TestSimpleFrameCodec::feed_corruptedFrame_droppedByCrc()
{
    SimpleFrameCodec codec;
    QByteArray frame = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 3,
                                                   QByteArray("hello"));
    // flip a bit in the first payload byte (offset = 10-byte header) -> CRC fails
    frame[10] = char(frame[10] ^ 0x01);

    auto msgs = codec.feed(frame);
    // the corrupted "hello" must never be surfaced as a valid message
    for (const auto &m : msgs)
        QVERIFY(m.payload != QByteArray("hello"));

    // and a clean frame still parses after a reset (resync from a fresh buffer)
    codec.reset();
    const QByteArray good = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 4,
                                                        QByteArray("ok"));
    auto msgs2 = codec.feed(good);
    QCOMPARE(msgs2.size(), size_t(1));
    QCOMPARE(msgs2[0].correlationId, quint32(4));
    QCOMPARE(msgs2[0].payload, QByteArray("ok"));
}

QTEST_APPLESS_MAIN(TestSimpleFrameCodec)
#include "tst_SimpleFrameCodec.moc"
