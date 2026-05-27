#include <QSignalSpy>
#include <QTest>
#include <memory>

#include <GChannelManager/Gateway.h>
#include <GChannelManager/SimpleFrameCodec.h>

#include "FakeTransport.h"

namespace {

// Хелпер: создаёт gw + кодек + FakeTransport, возвращает указатель на транспорт
// (gw владеет им через unique_ptr).
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

// Из самой свежей записи в transport->sent() вытащить corrId последнего Request-кадра.
// Возвращает 0, если такого нет.
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
};

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

void TestGateway::session_reachesActive_afterKeepAliveReply()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Establishing);
    QVERIFY(!t->sent().isEmpty());   // первый keep-alive отправлен сразу

    replyKeepAlive(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
}

void TestGateway::session_keepAliveDisabled_becomesActiveImmediately()
{
    Gateway gw;
    wireUp(gw, std::chrono::milliseconds(40), /*keepAliveEnabled=*/false);
    gw.enableChannel();
    gw.startSession();

    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
}

void TestGateway::setKeepAliveEnabled_runtimeOff_clearsSuspendedToActive()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    // в Establishing — линк ещё не подтверждён
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Establishing);

    QSignalSpy kaSpy(&gw, &Gateway::keepAliveEnabledChanged);
    gw.setKeepAliveEnabled(false);

    QCOMPARE(kaSpy.count(), 1);
    QCOMPARE(kaSpy.first().at(0).toBool(), false);
    // без heartbeat нечем подтверждать линк — Gateway считает сессию Active
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    QVERIFY(!gw.isKeepAliveEnabled());
    Q_UNUSED(t);
}

void TestGateway::setKeepAliveEnabled_runtimeOn_startsHeartbeat()
{
    Gateway gw;
    auto *t = wireUp(gw, std::chrono::milliseconds(40), /*keepAliveEnabled=*/false);
    gw.enableChannel();
    gw.startSession();
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    gw.setKeepAliveEnabled(true);
    // включение heartbeat немедленно отправляет первый keep-alive кадр
    QVERIFY(!t->sent().isEmpty());

    QByteArray buf = t->sent().last();
    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].type, quint8(SimpleFrameCodec::KeepAliveReq));
}

void TestGateway::sendRequest_succeedsOnPeerReply()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    auto *req = gw.sendRequest(QByteArray("ping"));
    QVERIFY(req);
    QSignalSpy okSpy(req, &GatewayRequest::succeeded);
    QSignalSpy doneSpy(req, &GatewayRequest::finished);

    // ждём, пока QTimer::singleShot(0,...) запустит первую попытку
    QTRY_VERIFY(!t->sent().isEmpty());

    const quint32 corrId = lastRequestCorrId(t);
    QVERIFY(corrId != 0);

    // узел отвечает
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, corrId,
                                                   QByteArray("ACK:ping")));

    QTRY_COMPARE(okSpy.count(), 1);
    QCOMPARE(okSpy.first().at(0).toByteArray(), QByteArray("ACK:ping"));
    QCOMPARE(doneSpy.count(), 1);
}

void TestGateway::sendRequest_retriesOnTimeout_thenSucceeds()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);
    t->clearSent();

    Gateway::RetryPolicy policy;
    policy.maxRetries    = 3;
    policy.timeout       = std::chrono::milliseconds(60);
    policy.backoffFactor = 1.0;   // без backoff — тест предсказуемее
    policy.maxTimeout    = std::chrono::milliseconds(500);

    auto *req = gw.sendRequest(QByteArray("ping"), policy);
    QSignalSpy retrySpy(req, &GatewayRequest::retrying);
    QSignalSpy okSpy(req,    &GatewayRequest::succeeded);

    // дождаться первой отправки и хотя бы одного retry
    QTRY_VERIFY(!t->sent().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(retrySpy.count() >= 1, true, 1000);

    // теперь узел отвечает на самый свежий corrId (он не меняется между попытками)
    const quint32 corrId = lastRequestCorrId(t);
    QVERIFY(corrId != 0);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, corrId,
                                                   QByteArray("late")));

    QTRY_COMPARE(okSpy.count(), 1);
    QVERIFY(req->attempts() >= 2);   // как минимум одна попытка-повтор
}

void TestGateway::sendRequest_failsBeforeChannelEnabled()
{
    Gateway gw;
    wireUp(gw);
    // канал НЕ включаем

    auto *req = gw.sendRequest(QByteArray("nope"));
    QSignalSpy failSpy(req, &GatewayRequest::failed);
    QTRY_COMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.first().at(0).value<GatewayRequest::Error>(),
             GatewayRequest::Error::ChannelDisabled);
}

void TestGateway::send_fireAndForget_emitsDataFrame()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
    t->clearSent();

    const bool ok = gw.send(QByteArray("hello"));
    QVERIFY(ok);
    QCOMPARE(t->sent().size(), 1);

    QByteArray buf = t->sent().first();
    auto frames = SimpleFrameCodec::parse(buf);
    QCOMPARE(frames.size(), size_t(1));
    QCOMPARE(frames[0].type,    quint8(SimpleFrameCodec::Data));
    QCOMPARE(frames[0].corrId,  quint32(0));   // корреляция отсутствует
    QCOMPARE(frames[0].payload, QByteArray("hello"));
}

void TestGateway::send_failsWhenSessionInactive()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    // сессию не стартуем

    QSignalSpy errSpy(&gw, &Gateway::errorOccurred);
    QCOMPARE(gw.send(QByteArray("x")), false);
    QVERIFY(errSpy.count() >= 1);
    QCOMPARE(t->sent().size(), 0);
}

void TestGateway::cancel_failsPendingRequest()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);

    Gateway::RetryPolicy slow;
    slow.maxRetries = 5;
    slow.timeout    = std::chrono::milliseconds(10'000);  // запрос точно не успеет таймаутнуть

    auto *req = gw.sendRequest(QByteArray("ping"), slow);
    QSignalSpy failSpy(req, &GatewayRequest::failed);

    req->cancel();
    QTRY_COMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.first().at(0).value<GatewayRequest::Error>(),
             GatewayRequest::Error::Cancelled);
}

void TestGateway::stats_countersTrackKeepAliveAndRequest()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();

    // первый keep-alive ушёл сразу же в onKeepAliveTick()
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

void TestGateway::stats_droppedReplyCounted()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);
    QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);

    // приходит Reply на несуществующий corrId — должен учесться как droppedReplies
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, 9999,
                                                   QByteArray("orphan")));
    QCOMPARE(gw.stats().droppedReplies, quint64(1));
}

void TestGateway::stats_periodicSignalFires_andStopsOnZeroInterval()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);

    QSignalSpy spy(&gw, &Gateway::statsUpdated);
    gw.setStatsInterval(std::chrono::milliseconds(30));

    QTRY_COMPARE_WITH_TIMEOUT(spy.count() >= 2, true, 1000);

    // снимок в сигнале должен быть валидным GatewayStats со счётчиками от текущей сессии
    const auto args = spy.last();
    QVERIFY(!args.isEmpty());
    const auto snap = args.first().value<GatewayStats>();
    QVERIFY(snap.keepAlivesSent >= 1);

    gw.setStatsInterval(std::chrono::milliseconds(0));
    spy.clear();
    QTest::qWait(120);
    QCOMPARE(spy.count(), 0);   // после 0-интервала эмиссия прекращена
}

void TestGateway::stats_resetClearsCounters()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);
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
//  Входящие запросы и кэш ответов
// ---------------------------------------------------------------------
void TestGateway::incomingRequest_emitsRequestReceived()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);

    QSignalSpy spy(&gw, &Gateway::requestReceived);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 42,
                                                   QByteArray("DO_IT")));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<quint32>(), quint32(42));
    QCOMPARE(spy.first().at(1).toByteArray(), QByteArray("DO_IT"));
    QCOMPARE(gw.stats().incomingRequests, quint64(1));
}

void TestGateway::reply_sendsReplyFrameViaTransport()
{
    Gateway gw;
    auto *t = wireUp(gw);
    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);
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

void TestGateway::replyCache_disabled_emitsSignalOnEveryRequest()
{
    Gateway gw;
    auto *t = wireUp(gw);
    // По умолчанию кэш выключен.
    QVERIFY(!gw.isReplyCacheEnabled());

    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);

    QSignalSpy spy(&gw, &Gateway::requestReceived);

    // первый запрос — обрабатываем, отвечаем
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 1,
                                                   QByteArray("A")));
    QCOMPARE(spy.count(), 1);
    QVERIFY(gw.reply(1, QByteArray("R1")));

    // дубль — без кэша приложение должно обработать его повторно
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 1,
                                                   QByteArray("A")));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(gw.stats().cachedRepliesResent, quint64(0));
}

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
    replyKeepAlive(t);

    QSignalSpy spy(&gw, &Gateway::requestReceived);

    // первый раз — приложение получает сигнал и отвечает
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 99,
                                                   QByteArray("X")));
    QCOMPARE(spy.count(), 1);
    QVERIFY(gw.reply(99, QByteArray("Y")));

    t->clearSent();

    // повтор — сигнал НЕ эмитится, в транспорт сразу уходит сохранённый ответ
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 99,
                                                   QByteArray("X")));
    QCOMPARE(spy.count(), 1);                   // не вырос
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

void TestGateway::replyCache_disableClearsExistingEntries()
{
    Gateway gw;
    auto *t = wireUp(gw);

    Gateway::ReplyCacheConfig cc;
    cc.enabled = true;
    gw.setReplyCacheConfig(cc);

    gw.enableChannel();
    gw.startSession();
    replyKeepAlive(t);

    // положим запись в кэш
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 5,
                                                   QByteArray("CMD")));
    QVERIFY(gw.reply(5, QByteArray("DONE")));

    QSignalSpy chSpy(&gw, &Gateway::replyCacheEnabledChanged);
    gw.setReplyCacheEnabled(false);
    QCOMPARE(chSpy.count(), 1);
    QCOMPARE(chSpy.first().at(0).toBool(), false);

    // снова включим — кэш должен оказаться пустым (disable очистил)
    gw.setReplyCacheEnabled(true);

    QSignalSpy reqSpy(&gw, &Gateway::requestReceived);
    t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Request, 5,
                                                   QByteArray("CMD")));
    QCOMPARE(reqSpy.count(), 1);   // снова эмитим — записи в кэше нет
}

QTEST_MAIN(TestGateway)
#include "tst_Gateway.moc"
