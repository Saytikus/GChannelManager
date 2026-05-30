#include <QSignalSpy>
#include <QTest>
#include <memory>

#include <GChannelManager/Gateway.h>
#include <GChannelManager/SimpleFrameCodec.h>

#include "FakeTransport.h"

// =====================================================================
//  Unit tests for Gateway, driven through the in-memory FakeTransport.
//  Each test wires a Gateway to a SimpleFrameCodec and a FakeTransport,
//  drives it via the public API, and asserts on emitted signals, the
//  frames pushed to the transport, and the statistics counters.
//  FakeTransport lets us "deliver" arbitrary peer frames synchronously,
//  while QTRY_* macros wait out the asynchronous (timer-driven) paths.
// =====================================================================

namespace {

// Helper: creates gw + codec + FakeTransport, returns a pointer to the transport
// (gw owns it via unique_ptr).
FakeTransport *wireUp(Gateway &gw,
                      std::chrono::milliseconds keepAliveInterval = std::chrono::milliseconds(40),
                      bool keepAliveEnabled = true)
{
    gw.setCodec(std::make_unique<SimpleFrameCodec>());
    auto transport = std::make_unique<FakeTransport>();
    auto *raw = transport.get();
    gw.setTransport(std::move(transport));

    Gateway::KeepAliveConfig ka;
    ka.enabled   = keepAliveEnabled;
    ka.interval  = keepAliveInterval;
    ka.maxMissed = 3;
    gw.setKeepAliveConfig(ka);
    return raw;
}

void replyKeepAlive(FakeTransport *t) {
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::KeepAliveReply, 0, {}));
}

// Delivers a SessionStartAck — the Gateway moves the session from Establishing to Active.
void ackSessionStart(FakeTransport *t) {
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStartAck, 0, {}));
}

// Extract the corrId of the most recent Request frame from transport->sent().
// Returns 0 if there is none.
quint32 lastRequestCorrId(const FakeTransport *t)
{
    for (qsizetype i = t->sent().size() - 1; i >= 0; --i) {
        QByteArray buf = t->sent().at(i);
        for (const auto &f : SimpleFrameCodec::parse(buf)) {
            if (f.type == SimpleFrameCodec::Request)
                return f.corrId;
        }
    }
    return 0;
}

// A transport that answers a Request *synchronously from inside send()* — the
// worst case for re-entrancy. It lets us exercise the path where a reply
// completes (and erases) a Pending request while PendingRequests::startAttempt
// is still mid-send (L4). deliver() injects arbitrary inbound bytes.
class SyncReplyTransport : public ITransport
{
    Q_OBJECT
public:
    using ITransport::ITransport;

    State   state() const override { return m_state; }
    QString name()  const override { return QStringLiteral("sync"); }

    void deliver(const QByteArray &bytes) { if (m_state == State::Open) emit bytesReceived(bytes); }

public slots:
    void open()  override { m_state = State::Open;   emit stateChanged(m_state); emit opened(); }
    void close() override { m_state = State::Closed; emit stateChanged(m_state); emit closed(); }
    qint64 send(const QByteArray &data) override
    {
        if (m_state != State::Open)
            return -1;
        // Re-enter the gateway: reply to any Request right now, before returning.
        QByteArray buf = data;
        for (const auto &f : SimpleFrameCodec::parse(buf)) {
            if (f.type == SimpleFrameCodec::Request)
                emit bytesReceived(SimpleFrameCodec::makeFrame(
                    SimpleFrameCodec::Reply, f.corrId, QByteArray("SYNC:") + f.payload));
        }
        return data.size();
    }

private:
    State m_state = State::Closed;
};

} // namespace

class TestGateway : public QObject
{
    Q_OBJECT
private slots:
    void channelEnable_emitsStateAndOpensTransport();
    void session_reachesActive_afterKeepAliveReply();
    void session_keepAliveDisabled_becomesActiveImmediately();
    void setKeepAliveEnabled_runtimeOff_clearsSuspendedToActive();
    void setKeepAliveEnabled_runtimeOn_startsHeartbeat();
    void sendRequest_succeedsOnPeerReply();
    void sendRequest_retriesOnTimeout_thenSucceeds();
    void sendRequest_synchronousReplyFromSend_completesSafely();
    void sendRequest_failsBeforeChannelEnabled();
    void send_fireAndForget_emitsDataFrame();
    void send_failsWhenSessionInactive();
    void cancel_failsPendingRequest();
    void stats_countersTrackKeepAliveAndRequest();
    void stats_droppedReplyCounted();
    void stats_periodicSignalFires_andStopsOnZeroInterval();
    void stats_resetClearsCounters();
    void incomingRequest_emitsRequestReceived();
    void reply_sendsReplyFrameViaTransport();
    void replyCache_disabled_emitsSignalOnEveryRequest();
    void replyCache_enabled_resendsCachedReplyWithoutEmittingSignal();
    void replyCache_disableClearsExistingEntries();
    void replyCache_clearedOnSessionRestart_freshRequestNotStale();
    void config_clampsInvalidValues();
    void startSession_sendsSessionStartFrame();
    void stopSession_sendsSessionStopFrame();
    void incomingSessionStart_acksAndEntersActive();
    void incomingSessionStop_failsPendingAndGoesIdle();
    void startSession_timeoutFiresWhenAckMissing();
};

// enableChannel() opens the transport and reports Enabled exactly once.
void TestGateway::channelEnable_emitsStateAndOpensTransport()
{
    Gateway gw;
    auto *t = wireUp(gw);

    QSignalSpy spy(&gw, &Gateway::channelStateChanged);
    gw.enableChannel();

    QCOMPARE(t->state(), ITransport::State::Open);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(gw.channelState(), Gateway::ChannelState::Enabled);
}

// startSession() emits SessionStart and reaches Active once the peer acks.
void TestGateway::session_reachesActive_afterKeepAliveReply()
{
    // The name is historical — the session now reaches Active via SessionStartAck.
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Establishing);

    // the first frame sent is SessionStart
    QVERIFY(!t->sent().isEmpty());
    QByteArray first = t->sent().first();
    auto frames = SimpleFrameCodec::parse(first);
    QVERIFY(!frames.empty());
    QCOMPARE(frames[0].type, quint8(SimpleFrameCodec::SessionStart));

    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
}

// With keep-alive disabled, the session still reaches Active and stays silent (no heartbeat).
void TestGateway::session_keepAliveDisabled_becomesActiveImmediately()
{
    // The name is historical — the handshake is mandatory regardless of keep-alive.
    // The test checks: after the ack, the heartbeat does NOT start if keep-alive is off.
    Gateway gw;
    auto *t = wireUp(gw, std::chrono::milliseconds(40), /*keepAliveEnabled=*/false);
    gw.enableChannel();
    gw.startSession();
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Establishing);

    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    t->clearSent();
    QTest::qWait(120);   // longer than several potential keep-alive ticks
    // nothing should go to the transport — the heartbeat is off
    QCOMPARE(t->sent().size(), 0);
}

// Disabling keep-alive at runtime while Suspended forces the session back to Active.
void TestGateway::setKeepAliveEnabled_runtimeOff_clearsSuspendedToActive()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    // do not answer keep-alive — we should land in Suspended (maxMissed=3, interval=40ms)
    QTRY_COMPARE_WITH_TIMEOUT(gw.sessionState(),
                              Gateway::SessionState::Suspended, 1500);

    QSignalSpy kaSpy(&gw, &Gateway::keepAliveEnabledChanged);
    gw.setKeepAliveEnabled(false);
    QCOMPARE(kaSpy.count(), 1);
    QCOMPARE(kaSpy.first().at(0).toBool(), false);
    // without a heartbeat there is nothing to detect a drop — the Gateway treats the session as Active
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    QVERIFY(!gw.isKeepAliveEnabled());
}

// Enabling keep-alive at runtime immediately emits the first heartbeat frame.
void TestGateway::setKeepAliveEnabled_runtimeOn_startsHeartbeat()
{
    Gateway gw;
    auto *t = wireUp(gw, std::chrono::milliseconds(40), /*keepAliveEnabled=*/false);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    gw.setKeepAliveEnabled(true);
    // enabling the heartbeat sends the first keep-alive frame right away
    QVERIFY(!t->sent().isEmpty());

    QByteArray buf = t->sent().last();
    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].type, quint8(SimpleFrameCodec::KeepAliveReq));
}

// A request succeeds when the peer's Reply carries the matching corrId.
void TestGateway::sendRequest_succeedsOnPeerReply()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    auto *req = gw.sendRequest(QByteArray("ping"));
    QVERIFY(req);
    QSignalSpy okSpy(req, &GatewayRequest::succeeded);
    QSignalSpy doneSpy(req, &GatewayRequest::finished);

    // wait until QTimer::singleShot(0,...) fires the first attempt
    QTRY_VERIFY(!t->sent().isEmpty());

    const quint32 corrId = lastRequestCorrId(t);
    QVERIFY(corrId != 0);

    // the peer answers
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, corrId,
                                                   QByteArray("ACK:ping")));

    QTRY_COMPARE(okSpy.count(), 1);
    QCOMPARE(okSpy.first().at(0).toByteArray(), QByteArray("ACK:ping"));
    QCOMPARE(doneSpy.count(), 1);
}

// A request retries on timeout (same corrId) and still succeeds on a late reply.
void TestGateway::sendRequest_retriesOnTimeout_thenSucceeds()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    t->clearSent();

    Gateway::RetryPolicy policy;
    policy.maxRetries    = 3;
    policy.timeout       = std::chrono::milliseconds(60);
    policy.backoffFactor = 1.0;   // no backoff — makes the test more predictable
    policy.maxTimeout    = std::chrono::milliseconds(500);

    auto *req = gw.sendRequest(QByteArray("ping"), policy);
    QSignalSpy retrySpy(req, &GatewayRequest::retrying);
    QSignalSpy okSpy(req,    &GatewayRequest::succeeded);

    // wait for the first send and at least one retry
    QTRY_VERIFY(!t->sent().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(retrySpy.count() >= 1, true, 1000);

    // now the peer answers the most recent corrId (it does not change between attempts)
    const quint32 corrId = lastRequestCorrId(t);
    QVERIFY(corrId != 0);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, corrId,
                                                   QByteArray("late")));

    QTRY_COMPARE(okSpy.count(), 1);
    QVERIFY(req->attempts() >= 2);   // at least one retry attempt
}

// A reply delivered synchronously from inside transport->send() must complete
// the request cleanly — no use-after-free in startAttempt (L4 regression).
void TestGateway::sendRequest_synchronousReplyFromSend_completesSafely()
{
    Gateway gw;
    gw.setCodec(std::make_unique<SimpleFrameCodec>());
    auto transport = std::make_unique<SyncReplyTransport>();
    auto *t = transport.get();
    gw.setTransport(std::move(transport));

    Gateway::KeepAliveConfig ka;
    ka.enabled = false;   // no heartbeat sends re-entering during this test
    gw.setKeepAliveConfig(ka);

    gw.enableChannel();
    // Bring the session Active via an incoming SessionStart (the ack we send is
    // not a Request, so it does not re-enter). Avoids the handshake round-trip.
    t->deliver(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStart, 0, {}));
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    auto *req = gw.sendRequest(QByteArray("ping"));
    QSignalSpy okSpy(req, &GatewayRequest::succeeded);

    // startAttempt fires on the next loop turn; the sync transport replies from
    // inside send(), so the request resolves without dangling its Pending entry.
    // (Read the result from the spy, not from req: it is deleteLater'd on
    // completion and is gone once QTRY has spun the event loop.)
    QTRY_COMPARE(okSpy.count(), 1);
    QCOMPARE(okSpy.first().at(0).toByteArray(), QByteArray("SYNC:ping"));
    QCOMPARE(gw.stats().requestsSucceeded, quint64(1));
}

// A request issued before the channel is enabled fails preflight with ChannelDisabled.
void TestGateway::sendRequest_failsBeforeChannelEnabled()
{
    Gateway gw;
    wireUp(gw);
    // do NOT enable the channel

    auto *req = gw.sendRequest(QByteArray("nope"));
    QSignalSpy failSpy(req, &GatewayRequest::failed);
    QTRY_COMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.first().at(0).value<GatewayRequest::Error>(),
             GatewayRequest::Error::ChannelDisabled);
}

// send() emits a single uncorrelated Data frame (corrId == 0).
void TestGateway::send_fireAndForget_emitsDataFrame()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    const bool ok = gw.send(QByteArray("hello"));
    QVERIFY(ok);
    QCOMPARE(t->sent().size(), 1);

    QByteArray buf = t->sent().first();
    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].type,    quint8(SimpleFrameCodec::Data));
    QCOMPARE(frames[0].corrId,  quint32(0));   // no correlation
    QCOMPARE(frames[0].payload, QByteArray("hello"));
}

// send() refuses (and emits errorOccurred) when there is no active session.
void TestGateway::send_failsWhenSessionInactive()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    // do not start the session

    QSignalSpy errSpy(&gw, &Gateway::errorOccurred);
    QCOMPARE(gw.send(QByteArray("x")), false);
    QVERIFY(errSpy.count() >= 1);
    QCOMPARE(t->sent().size(), 0);
}

// cancel() fails a still-pending request with Error::Cancelled.
void TestGateway::cancel_failsPendingRequest()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    Gateway::RetryPolicy slow;
    slow.maxRetries = 5;
    slow.timeout    = std::chrono::milliseconds(10'000);  // the request will definitely not time out

    auto *req = gw.sendRequest(QByteArray("ping"), slow);
    QSignalSpy failSpy(req, &GatewayRequest::failed);

    req->cancel();
    QTRY_COMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.first().at(0).value<GatewayRequest::Error>(),
             GatewayRequest::Error::Cancelled);
}

// Keep-alive and request counters track a full heartbeat + round-trip.
void TestGateway::stats_countersTrackKeepAliveAndRequest()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    // the first keep-alive went out immediately in enterActiveState()
    QVERIFY(gw.stats().keepAlivesSent >= 1);
    QCOMPARE(gw.stats().keepAlivesReceived, quint64(0));

    replyKeepAlive(t);
    QCOMPARE(gw.stats().keepAlivesReceived, quint64(1));
    t->clearSent();

    auto *req = gw.sendRequest(QByteArray("ping"));
    QSignalSpy ok(req, &GatewayRequest::succeeded);
    QTRY_VERIFY(lastRequestCorrId(t) != 0);
    const quint32 cid = lastRequestCorrId(t);

    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, cid,
                                                   QByteArray("ACK")));
    QTRY_COMPARE(ok.count(), 1);

    const auto s = gw.stats();
    QCOMPARE(s.requestsSent,      quint64(1));
    QCOMPARE(s.requestsSucceeded, quint64(1));
    QCOMPARE(s.requestsFailed,    quint64(0));
    QVERIFY(s.sentBytes > 0);
    QVERIFY(s.recvBytes > 0);
}

// A Reply for an unknown corrId is counted as a dropped reply.
void TestGateway::stats_droppedReplyCounted()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    // a Reply for a non-existent corrId arrives — counted as droppedReplies and
    // NOT surfaced as dataReceived (duplicates on a lossy link must not pose as data)
    QSignalSpy dataSpy(&gw, &Gateway::dataReceived);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 9999,
                                                   QByteArray("orphan")));
    QCOMPARE(gw.stats().droppedReplies, quint64(1));
    QCOMPARE(dataSpy.count(), 0);
}

// The periodic statsUpdated signal fires on a timer and stops at interval 0.
void TestGateway::stats_periodicSignalFires_andStopsOnZeroInterval()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    QSignalSpy spy(&gw, &Gateway::statsUpdated);
    gw.setStatsInterval(std::chrono::milliseconds(30));

    QTRY_COMPARE_WITH_TIMEOUT(spy.count() >= 2, true, 1000);

    // the snapshot in the signal must be a valid GatewayStats with counters from the current session
    const auto args = spy.last();
    QVERIFY(!args.isEmpty());
    const auto snap = args.first().value<GatewayStats>();
    QVERIFY(snap.keepAlivesSent >= 1);

    gw.setStatsInterval(std::chrono::milliseconds(0));
    spy.clear();
    QTest::qWait(120);
    QCOMPARE(spy.count(), 0);   // after a 0 interval, emission has stopped
}

// resetStats() zeros every counter.
void TestGateway::stats_resetClearsCounters()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QVERIFY(gw.stats().keepAlivesSent >= 1);

    gw.resetStats();
    const auto s = gw.stats();
    QCOMPARE(s.sentBytes,          quint64(0));
    QCOMPARE(s.recvBytes,          quint64(0));
    QCOMPARE(s.keepAlivesSent,     quint64(0));
    QCOMPARE(s.keepAlivesReceived, quint64(0));
    QCOMPARE(s.requestsSent,       quint64(0));
}

// ---------------------------------------------------------------------
//  Incoming requests and the reply cache
// ---------------------------------------------------------------------

// An incoming Request frame emits requestReceived and bumps incomingRequests.
void TestGateway::incomingRequest_emitsRequestReceived()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    QSignalSpy spy(&gw, &Gateway::requestReceived);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 42,
                                                   QByteArray("DO_IT")));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<quint32>(), quint32(42));
    QCOMPARE(spy.first().at(1).toByteArray(), QByteArray("DO_IT"));
    QCOMPARE(gw.stats().incomingRequests, quint64(1));
}

// reply() encodes a Reply frame (correct corrId/payload) and sends it.
void TestGateway::reply_sendsReplyFrameViaTransport()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    t->clearSent();

    QVERIFY(gw.reply(7, QByteArray("DONE")));
    QCOMPARE(t->sent().size(), 1);

    QByteArray buf = t->sent().first();
    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].type,    quint8(SimpleFrameCodec::Reply));
    QCOMPARE(frames[0].corrId,  quint32(7));
    QCOMPARE(frames[0].payload, QByteArray("DONE"));
}

// With the cache off, a duplicate Request is delivered to the app every time.
void TestGateway::replyCache_disabled_emitsSignalOnEveryRequest()
{
    Gateway gw;
    auto *t = wireUp(gw);
    // The cache is disabled by default.
    QVERIFY(!gw.isReplyCacheEnabled());

    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    QSignalSpy spy(&gw, &Gateway::requestReceived);

    // first request — handle it, reply
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 1,
                                                   QByteArray("A")));
    QCOMPARE(spy.count(), 1);
    QVERIFY(gw.reply(1, QByteArray("R1")));

    // duplicate — without a cache the app must process it again
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 1,
                                                   QByteArray("A")));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(gw.stats().cachedRepliesResent, quint64(0));
}

// With the cache on, a duplicate Request resends the stored reply without re-emitting.
void TestGateway::replyCache_enabled_resendsCachedReplyWithoutEmittingSignal()
{
    Gateway gw;
    auto *t = wireUp(gw);

    Gateway::ReplyCacheConfig cc;
    cc.enabled    = true;
    cc.maxEntries = 16;
    gw.setReplyCacheConfig(cc);
    QVERIFY(gw.isReplyCacheEnabled());

    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    QSignalSpy spy(&gw, &Gateway::requestReceived);

    // the first time — the app gets the signal and replies
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 99,
                                                   QByteArray("X")));
    QCOMPARE(spy.count(), 1);
    QVERIFY(gw.reply(99, QByteArray("Y")));

    t->clearSent();

    // duplicate — the signal is NOT emitted, the stored reply goes straight to the transport
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 99,
                                                   QByteArray("X")));
    QCOMPARE(spy.count(), 1);                   // did not grow
    QCOMPARE(t->sent().size(), 1);

    QByteArray buf = t->sent().first();
    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].type,    quint8(SimpleFrameCodec::Reply));
    QCOMPARE(frames[0].corrId,  quint32(99));
    QCOMPARE(frames[0].payload, QByteArray("Y"));

    QCOMPARE(gw.stats().cachedRepliesResent, quint64(1));
    QCOMPARE(gw.stats().incomingRequests,    quint64(2));
}

// Disabling the cache clears its entries (a later duplicate is delivered again).
void TestGateway::replyCache_disableClearsExistingEntries()
{
    Gateway gw;
    auto *t = wireUp(gw);

    Gateway::ReplyCacheConfig cc;
    cc.enabled = true;
    gw.setReplyCacheConfig(cc);

    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);

    // put an entry into the cache
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 5,
                                                   QByteArray("CMD")));
    QVERIFY(gw.reply(5, QByteArray("DONE")));

    QSignalSpy chSpy(&gw, &Gateway::replyCacheEnabledChanged);
    gw.setReplyCacheEnabled(false);
    QCOMPARE(chSpy.count(), 1);
    QCOMPARE(chSpy.first().at(0).toBool(), false);

    // enable it again — the cache must be empty (disable cleared it)
    gw.setReplyCacheEnabled(true);

    QSignalSpy reqSpy(&gw, &Gateway::requestReceived);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 5,
                                                   QByteArray("CMD")));
    QCOMPARE(reqSpy.count(), 1);   // emitted again — no entry in the cache
}

// After a session restart the reply cache is cleared, so a peer that restarts
// its corrIds does not get a stale reply: a *different* request reusing an old
// corrId must surface via requestReceived, not a silent cached resend.
void TestGateway::replyCache_clearedOnSessionRestart_freshRequestNotStale()
{
    Gateway gw;
    auto *t = wireUp(gw);

    Gateway::ReplyCacheConfig cc;
    cc.enabled = true;
    gw.setReplyCacheConfig(cc);

    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    // Session 1: peer asks with corrId=1, we answer -> reply cached at corrId=1.
    QSignalSpy reqSpy(&gw, &Gateway::requestReceived);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 1,
                                                   QByteArray("GET /a")));
    QCOMPARE(reqSpy.count(), 1);
    QVERIFY(gw.reply(1, QByteArray("REPLY-A")));

    // The session ends and a new one is established (the peer restarts corrIds).
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStop, 0, {}));
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Idle);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStart, 0, {}));
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    t->clearSent();

    // Session 2: a DIFFERENT request reuses corrId=1. The cache was cleared, so
    // the app must see it (not a silent stale resend of REPLY-A).
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 1,
                                                   QByteArray("GET /b")));
    QCOMPARE(reqSpy.count(), 2);
    QCOMPARE(reqSpy.last().at(1).toByteArray(), QByteArray("GET /b"));
    QCOMPARE(gw.stats().cachedRepliesResent, quint64(0));
}

// Invalid config values are clamped to safe ranges rather than silently
// breaking the keep-alive / reply-cache machinery.
void TestGateway::config_clampsInvalidValues()
{
    Gateway gw;

    // maxMissed < 0 would trip Suspended on the first heartbeat -> clamp to 0.
    Gateway::KeepAliveConfig ka;
    ka.enabled   = true;
    ka.maxMissed = -5;
    gw.setKeepAliveConfig(ka);
    QCOMPARE(gw.keepAliveConfig().maxMissed, 0);

    // maxEntries <= 0 makes QCache store nothing while appearing enabled -> clamp to >= 1.
    Gateway::ReplyCacheConfig cc;
    cc.enabled    = true;
    cc.maxEntries = 0;
    gw.setReplyCacheConfig(cc);
    QVERIFY(gw.replyCacheConfig().maxEntries >= 1);
}

// ---------------------------------------------------------------------
//  Session lifecycle: dedicated SessionStart/Stop frames
// ---------------------------------------------------------------------

// startSession() sends a SessionStart frame and enters Establishing.
void TestGateway::startSession_sendsSessionStartFrame()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    t->clearSent();

    gw.startSession();

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Establishing);
    QCOMPARE(gw.stats().sessionStartsSent, quint64(1));

    // the first frame on the transport is SessionStart
    QVERIFY(!t->sent().isEmpty());
    QByteArray buf = t->sent().first();
    auto frames = SimpleFrameCodec::parse(buf);
    QVERIFY(!frames.empty());
    QCOMPARE(frames[0].type, quint8(SimpleFrameCodec::SessionStart));
}

// stopSession() sends a SessionStop frame and returns to Idle.
void TestGateway::stopSession_sendsSessionStopFrame()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    gw.stopSession();

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Idle);
    QCOMPARE(gw.stats().sessionStopsSent, quint64(1));

    // there must be a SessionStop frame on the transport
    bool foundStop = false;
    for (const auto &raw : t->sent()) {
        QByteArray b = raw;
        for (const auto &f : SimpleFrameCodec::parse(b)) {
            if (f.type == SimpleFrameCodec::SessionStop) {
                foundStop = true;
                break;
            }
        }
    }
    QVERIFY(foundStop);
}

// An incoming SessionStart auto-acks and brings us to Active (server-role open).
void TestGateway::incomingSessionStart_acksAndEntersActive()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Idle);

    QSignalSpy startSpy(&gw, &Gateway::sessionStartReceived);
    t->clearSent();

    // the peer initiates a session with us
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStart, 0, {}));

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    QCOMPARE(startSpy.count(), 1);
    QCOMPARE(gw.stats().sessionStartsReceived, quint64(1));

    // we must send a SessionStartAck in reply
    bool foundAck = false;
    for (const auto &raw : t->sent()) {
        QByteArray b = raw;
        for (const auto &f : SimpleFrameCodec::parse(b)) {
            if (f.type == SimpleFrameCodec::SessionStartAck) {
                foundAck = true;
                break;
            }
        }
    }
    QVERIFY(foundAck);
}

// An incoming SessionStop fails pending requests and returns to Idle.
void TestGateway::incomingSessionStop_failsPendingAndGoesIdle()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    ackSessionStart(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    // launch a long-running request
    Gateway::RetryPolicy slow;
    slow.maxRetries = 10;
    slow.timeout    = std::chrono::seconds(10);
    auto *req = gw.sendRequest(QByteArray("ping"), slow);
    QSignalSpy failSpy(req, &GatewayRequest::failed);
    QSignalSpy stopSpy(&gw, &Gateway::sessionStopReceived);

    // the peer sends SessionStop
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStop, 0, {}));

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Idle);
    QCOMPARE(stopSpy.count(), 1);
    QCOMPARE(gw.stats().sessionStopsReceived, quint64(1));

    // the pending request must be failed with SessionInactive
    QTRY_COMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.first().at(0).value<GatewayRequest::Error>(),
             GatewayRequest::Error::SessionInactive);
}

// If no SessionStartAck arrives, the handshake times out back to Idle.
void TestGateway::startSession_timeoutFiresWhenAckMissing()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.setSessionStartTimeout(std::chrono::milliseconds(80));   // fast timeout
    gw.enableChannel();

    QSignalSpy stateSpy(&gw, &Gateway::sessionStateChanged);
    QSignalSpy errSpy(&gw,  &Gateway::errorOccurred);

    gw.startSession();
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Establishing);

    // do not answer — wait for the timeout
    QTRY_COMPARE_WITH_TIMEOUT(gw.sessionState(),
                              Gateway::SessionState::Idle, 1000);
    QCOMPARE(gw.stats().sessionStartTimeouts, quint64(1));
    QVERIFY(errSpy.count() >= 1);
    Q_UNUSED(t);
}

QTEST_MAIN(TestGateway)
#include "tst_Gateway.moc"
