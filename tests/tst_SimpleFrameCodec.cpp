#include <QTest>

#include <GChannelManager/SimpleFrameCodec.h>

class TestSimpleFrameCodec : public QObject
{
    Q_OBJECT
private slots:
    void encodeRequest_roundtrip();
    void incomingRequestFrame_classifiedAsTypeRequest();
    void encodeReply_carriesCorrelation();
    void encodeData_carriesNoCorrelation();
    void encodeKeepAlive_classifiedAsKeepAliveReplyOnly();
    void feed_splitAcrossCalls_buffered();
    void feed_garbagePrefix_resyncsByMagic();
    void feed_multipleFrames_inOneBuffer();
    void parse_consumesBytesIncrementally();
    void reset_clearsBuffer();
};

void TestSimpleFrameCodec::encodeRequest_roundtrip()
{
    SimpleFrameCodec codec;
    const QByteArray payload = "hello-world";
    const QByteArray frame   = codec.encodeRequest(42, payload);

    // Кадр запроса прилетел "обратно" в виде Reply кадра с тем же corrId —
    // именно так выглядит ответ узла на наш запрос. feed() классифицирует его как Reply.
    const QByteArray reply = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 42, payload);
    const auto msgs = codec.feed(reply);

    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Reply);
    QCOMPARE(msgs[0].correlationId, quint32(42));
    QCOMPARE(msgs[0].payload, payload);

    // А сама форма Request-кадра должна нести corrId и payload, готовые к отправке.
    QVERIFY(frame.size() >= 10);
    QCOMPARE(quint8(frame[0]), quint8(0xA5));
    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::Request));
}

void TestSimpleFrameCodec::incomingRequestFrame_classifiedAsTypeRequest()
{
    SimpleFrameCodec codec;
    // Узел шлёт нам Request — feed() должен классифицировать его как Type::Request,
    // не как Unknown (ранее библиотека была чисто клиентом и игнорировала такие кадры).
    const QByteArray frame = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 7,
                                                         QByteArray("DO_IT"));
    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Request);
    QCOMPARE(msgs[0].correlationId, quint32(7));
    QCOMPARE(msgs[0].payload, QByteArray("DO_IT"));
}

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

void TestSimpleFrameCodec::encodeData_carriesNoCorrelation()
{
    SimpleFrameCodec codec;
    const QByteArray payload = "push";
    const QByteArray frame   = codec.encodeData(payload);

    QCOMPARE(quint8(frame[1]), quint8(SimpleFrameCodec::Data));
    // corrId должен быть 0 (мы не корреллируем fire-and-forget)
    QCOMPARE(quint8(frame[2]), quint8(0));
    QCOMPARE(quint8(frame[3]), quint8(0));
    QCOMPARE(quint8(frame[4]), quint8(0));
    QCOMPARE(quint8(frame[5]), quint8(0));

    const auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Data);
    QCOMPARE(msgs[0].payload, payload);
}

void TestSimpleFrameCodec::encodeKeepAlive_classifiedAsKeepAliveReplyOnly()
{
    SimpleFrameCodec codec;
    // encodeKeepAlive() — это запрос heartbeat (KeepAliveReq), feed() его помечает Unknown
    // (потому что узлу не нужно реагировать на свой же запрос).
    const QByteArray req = codec.encodeKeepAlive();
    auto msgs = codec.feed(req);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::Unknown);

    // А вот KeepAliveReply от узла должен превратиться в DecodedMessage::Type::KeepAlive.
    const QByteArray rep = SimpleFrameCodec::makeFrame(SimpleFrameCodec::KeepAliveReply, 0, {});
    msgs = codec.feed(rep);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].type, DecodedMessage::Type::KeepAlive);
}

void TestSimpleFrameCodec::feed_splitAcrossCalls_buffered()
{
    SimpleFrameCodec codec;
    const QByteArray full = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 7,
                                                        QByteArray("abcdefgh"));
    QVERIFY(full.size() > 6);

    // Первый кусок — только заголовок без полного payload: ничего разобрать нельзя.
    auto msgs1 = codec.feed(full.left(6));
    QCOMPARE(msgs1.size(), size_t(0));

    // Второй кусок — остаток: появляется одно сообщение.
    auto msgs2 = codec.feed(full.mid(6));
    QCOMPARE(msgs2.size(), size_t(1));
    QCOMPARE(msgs2[0].correlationId, quint32(7));
    QCOMPARE(msgs2[0].payload, QByteArray("abcdefgh"));
}

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

void TestSimpleFrameCodec::parse_consumesBytesIncrementally()
{
    QByteArray buf = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 9, QByteArray("X"));
    buf += "tail";   // мусорный хвост, не образующий кадр

    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].corrId, quint32(9));

    // потреблённые байты должны исчезнуть, "tail" остаться в буфере на следующий feed
    QCOMPARE(buf, QByteArray("tail"));
}

void TestSimpleFrameCodec::reset_clearsBuffer()
{
    SimpleFrameCodec codec;
    // отправим частичный заголовок — он осядет во внутреннем буфере
    QByteArray partial = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 1,
                                                     QByteArray("AB")).left(4);
    QCOMPARE(codec.feed(partial).size(), size_t(0));

    codec.reset();

    // после reset внутренний буфер чист — следующий нормальный кадр парсится с нуля
    const QByteArray frame = SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 2,
                                                         QByteArray("CD"));
    auto msgs = codec.feed(frame);
    QCOMPARE(msgs.size(), size_t(1));
    QCOMPARE(msgs[0].correlationId, quint32(2));
}

QTEST_APPLESS_MAIN(TestSimpleFrameCodec)
#include "tst_SimpleFrameCodec.moc"
