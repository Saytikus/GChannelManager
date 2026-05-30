#include <QRandomGenerator>
#include <QTest>
#include <vector>

#include <GChannelManager/SimpleFrameCodec.h>

// =====================================================================
//  Property-based tests for SimpleFrameCodec.
//
//  Unlike tst_SimpleFrameCodec (hand-picked example frames), these assert
//  invariants over many randomized inputs from a *fixed-seed* generator, so
//  every run is deterministic and a failure reproduces verbatim (the seed is
//  printed in initTestCase). No fuzzing/property-test dependency — just a
//  seeded QRandomGenerator inside Qt Test.
//
//  Properties:
//    * round-trip: feed(makeFrame(type, corrId, payload)) yields one message
//      with the same fields (catches CRC/offset regressions);
//    * chunk-boundary invariance: the same bytes split at arbitrary boundaries
//      decode to the same sequence as one feed() (the core streaming property);
//    * resync recovery: valid frames separated by non-0xA5 garbage are all
//      recovered, in order;
//    * single-bit-flip rejection: flipping any one bit of a valid frame means
//      it is never delivered as the original (CRC + length bounding).
// =====================================================================

namespace {

constexpr quint32 kSeed = 0xC0DECAFEu;

constexpr SimpleFrameCodec::Type kTypes[] = {
    SimpleFrameCodec::Request,        SimpleFrameCodec::Reply,
    SimpleFrameCodec::KeepAliveReq,   SimpleFrameCodec::KeepAliveReply,
    SimpleFrameCodec::Data,           SimpleFrameCodec::SessionStart,
    SimpleFrameCodec::SessionStartAck, SimpleFrameCodec::SessionStop,
};

// Mirror of SimpleFrameCodec::feed()'s type classification.
DecodedMessage::Type expectedType(SimpleFrameCodec::Type t)
{
    switch (t) {
    case SimpleFrameCodec::Request:         return DecodedMessage::Type::Request;
    case SimpleFrameCodec::Reply:           return DecodedMessage::Type::Reply;
    case SimpleFrameCodec::KeepAliveReq:    return DecodedMessage::Type::KeepAlivePing;
    case SimpleFrameCodec::KeepAliveReply:  return DecodedMessage::Type::KeepAlive;
    case SimpleFrameCodec::Data:            return DecodedMessage::Type::Data;
    case SimpleFrameCodec::SessionStart:    return DecodedMessage::Type::SessionStart;
    case SimpleFrameCodec::SessionStartAck: return DecodedMessage::Type::SessionStartAck;
    case SimpleFrameCodec::SessionStop:     return DecodedMessage::Type::SessionStop;
    }
    return DecodedMessage::Type::Unknown;
}

SimpleFrameCodec::Type randomType(QRandomGenerator &rng)
{
    return kTypes[rng.bounded(int(std::size(kTypes)))];
}

QByteArray randomBytes(QRandomGenerator &rng, int maxLen)
{
    const int n = rng.bounded(maxLen + 1);
    QByteArray b(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i)
        b[i] = char(quint8(rng.bounded(256)));
    return b;
}

// A random frame plus the fields it was built from, for assertions.
struct SampleFrame {
    SimpleFrameCodec::Type type;
    quint32                corrId;
    QByteArray             payload;
    QByteArray             bytes;
};

SampleFrame randomFrame(QRandomGenerator &rng, int maxPayload)
{
    SampleFrame f;
    f.type    = randomType(rng);
    f.corrId  = rng.generate();
    f.payload = randomBytes(rng, maxPayload);
    f.bytes   = SimpleFrameCodec::makeFrame(quint8(f.type), f.corrId, f.payload);
    return f;
}

} // namespace

class TestCodecProperties : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qInfo("property tests use fixed seed 0x%08X — re-run reproduces any failure", kSeed);
    }

    void roundTrip_anyFrameDecodesToItsFields();
    void chunkBoundary_sameDecodeRegardlessOfSplit();
    void resync_recoversFramesSeparatedByGarbage();
    void singleBitFlip_isNeverDeliveredAsOriginal();
};

// feed(makeFrame(type, corrId, payload)) returns exactly one message whose
// type/corrId/payload match the inputs, for any random combination.
void TestCodecProperties::roundTrip_anyFrameDecodesToItsFields()
{
    QRandomGenerator rng(kSeed);
    for (int iter = 0; iter < 2000; ++iter) {
        const SampleFrame f = randomFrame(rng, 1024);
        SimpleFrameCodec codec;
        const auto msgs = codec.feed(f.bytes);
        QCOMPARE(msgs.size(), size_t(1));
        QCOMPARE(msgs[0].type, expectedType(f.type));
        QCOMPARE(msgs[0].correlationId, f.corrId);
        QCOMPARE(msgs[0].payload, f.payload);
    }
}

// The same total bytes decode to the same message sequence whether fed in one
// call or split at arbitrary chunk boundaries — the key streaming guarantee.
void TestCodecProperties::chunkBoundary_sameDecodeRegardlessOfSplit()
{
    QRandomGenerator rng(kSeed);
    for (int iter = 0; iter < 400; ++iter) {
        QByteArray whole;
        const int frames = 1 + rng.bounded(6);
        for (int i = 0; i < frames; ++i)
            whole += randomFrame(rng, 256).bytes;

        SimpleFrameCodec one;
        const auto ref = one.feed(whole);

        SimpleFrameCodec split;
        std::vector<DecodedMessage> got;
        int pos = 0;
        while (pos < whole.size()) {
            const int n = 1 + rng.bounded(int(whole.size()) - pos);
            const auto part = split.feed(whole.mid(pos, n));
            got.insert(got.end(), part.begin(), part.end());
            pos += n;
        }

        QCOMPARE(got.size(), ref.size());
        for (size_t i = 0; i < ref.size(); ++i) {
            QCOMPARE(got[i].type, ref[i].type);
            QCOMPARE(got[i].correlationId, ref[i].correlationId);
            QCOMPARE(got[i].payload, ref[i].payload);
        }
    }
}

// Valid frames separated by random non-magic garbage are all recovered in
// order — the parser resyncs past noise without losing real frames.
void TestCodecProperties::resync_recoversFramesSeparatedByGarbage()
{
    QRandomGenerator rng(kSeed);
    for (int iter = 0; iter < 400; ++iter) {
        const int frames = 1 + rng.bounded(6);
        std::vector<SampleFrame> samples;
        QByteArray stream;
        for (int i = 0; i < frames; ++i) {
            // leading garbage with no 0xA5 (so it cannot start a frame)
            const int g = rng.bounded(33);
            for (int k = 0; k < g; ++k) {
                quint8 b = quint8(rng.bounded(255));
                if (b >= 0xA5) ++b;            // map into [0..0xA4] U [0xA6..0xFF]
                stream.append(char(b));
            }
            SampleFrame f = randomFrame(rng, 256);
            samples.push_back(f);
            stream += f.bytes;
        }

        SimpleFrameCodec codec;
        const auto msgs = codec.feed(stream);

        QCOMPARE(msgs.size(), samples.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            QCOMPARE(msgs[i].type, expectedType(samples[i].type));
            QCOMPARE(msgs[i].correlationId, samples[i].corrId);
            QCOMPARE(msgs[i].payload, samples[i].payload);
        }
    }
}

// Flipping any single bit of a valid frame means the original frame is never
// decoded (CRC catches content/CRC flips; a magic/length flip breaks framing).
void TestCodecProperties::singleBitFlip_isNeverDeliveredAsOriginal()
{
    QRandomGenerator rng(kSeed);
    for (int iter = 0; iter < 2000; ++iter) {
        const SampleFrame f = randomFrame(rng, 256);
        QByteArray corrupted = f.bytes;

        const int bit = rng.bounded(int(corrupted.size()) * 8);
        corrupted[bit / 8] = char(quint8(corrupted[bit / 8]) ^ quint8(1u << (bit % 8)));

        SimpleFrameCodec codec;
        const auto msgs = codec.feed(corrupted);

        const DecodedMessage::Type want = expectedType(f.type);
        for (const auto &m : msgs) {
            const bool isOriginal = m.type == want
                                 && m.correlationId == f.corrId
                                 && m.payload == f.payload;
            QVERIFY2(!isOriginal, "a single-bit-flipped frame was delivered as the original");
        }
    }
}

QTEST_APPLESS_MAIN(TestCodecProperties)
#include "tst_CodecProperties.moc"
