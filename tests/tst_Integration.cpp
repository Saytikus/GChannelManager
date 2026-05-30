#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <memory>

#include <GChannelManager/Gateway.h>
#include <GChannelManager/SimpleFrameCodec.h>

// =====================================================================
//  Integration tests.
//
//  Unlike tst_Gateway (which drives ONE Gateway through a FakeTransport
//  and hand-crafted peer frames), these tests wire TWO real Gateway
//  instances back-to-back through a pair of in-memory transports and let
//  them talk to each other end to end. Nothing is mocked above the byte
//  pipe: both gateways run their own codec, handshake, correlation and
//  retry machinery, and exchange real SimpleFrameCodec frames.
//
//  This exercises the parts that single-Gateway unit tests cannot:
//    * the full SessionStart -> SessionStartAck handshake across the wire,
//    * request/reply round-trips in BOTH directions (the gateway is a
//      symmetric peer — either side can be the requester),
//    * fire-and-forget push from one side to the other,
//    * retry recovery when the link drops frames,
//    * mutual keep-alive: each gateway auto-answers the peer's KeepAliveReq,
//      so two bare gateways keep each other alive.
//
//  Most tests disable keep-alive to keep timing deterministic and focus on the
//  session + messaging paths; keepAlive_bothEndsStayActive covers the heartbeat
//  round-trip explicitly.
// =====================================================================

namespace {

// ---------------------------------------------------------------------
//  LinkedTransport — a plain asynchronous byte pipe between two gateways.
//
//  Two instances are paired via setPeer(). A send() on one schedules
//  delivery of the same bytes to the other's bytesReceived() on the next
//  event-loop turn (QTimer::singleShot), mimicking a real asynchronous
//  link and avoiding synchronous re-entrancy. It can also deterministically
//  "drop" the next N deliveries to simulate packet loss for retry tests.
// ---------------------------------------------------------------------
class LinkedTransport : public ITransport
{
    Q_OBJECT
public:
    using ITransport::ITransport;

    void setPeer(LinkedTransport *peer) { m_peer = peer; }
    void setName(QString name)          { m_name = std::move(name); }

    // Drop the next `n` outgoing frames (they are accepted by send() but
    // never reach the peer) — used to force the sender's retry path.
    void dropNextSends(int n) { m_dropNext = n; }

    // Called by the peer's send() to hand us inbound bytes.
    void deliverFromPeer(const QByteArray &bytes)
    {
        if (m_state == State::Open)
            emit bytesReceived(bytes);
    }

    State   state() const override { return m_state; }
    QString name()  const override { return m_name; }

public slots:
    void open() override
    {
        m_state = State::Open;
        emit stateChanged(m_state);
        emit opened();
    }
    void close() override
    {
        m_state = State::Closed;
        emit stateChanged(m_state);
        emit closed();
    }
    qint64 send(const QByteArray &data) override
    {
        if (m_state != State::Open)
            return -1;

        // The link accepted the bytes; whether they arrive is the link's business.
        if (m_dropNext > 0) {
            --m_dropNext;
            return data.size();             // silently lost in transit
        }
        if (m_peer) {
            LinkedTransport *peer = m_peer;
            const QByteArray copy = data;
            QTimer::singleShot(0, peer, [peer, copy] { peer->deliverFromPeer(copy); });
        }
        return data.size();
    }

private:
    State            m_state   = State::Closed;
    QString          m_name    = QStringLiteral("linked");
    LinkedTransport *m_peer    = nullptr;
    int              m_dropNext = 0;
};

// Build a gateway with a SimpleFrameCodec and the given LinkedTransport,
// keep-alive disabled (see the file header for why). Returns the raw
// transport pointer (the gateway owns it via unique_ptr).
LinkedTransport *equip(Gateway &gw, std::unique_ptr<LinkedTransport> transport)
{
    auto *raw = transport.get();
    gw.setCodec(std::make_unique<SimpleFrameCodec>());
    gw.setTransport(std::move(transport));

    Gateway::KeepAliveConfig ka;
    ka.enabled = false;
    gw.setKeepAliveConfig(ka);
    return raw;
}

} // namespace

class TestIntegration : public QObject
{
    Q_OBJECT
private:
    // Two gateways and their paired link. Rebuilt fresh for every test in init().
    std::unique_ptr<Gateway> m_client;
    std::unique_ptr<Gateway> m_server;
    LinkedTransport         *m_clientLink = nullptr;   // owned by m_client
    LinkedTransport         *m_serverLink = nullptr;   // owned by m_server

    // Bring both ends up to an Active session: client initiates, server
    // auto-acks the incoming SessionStart. Returns once both are Active.
    void establishSession()
    {
        m_client->enableChannel();
        m_server->enableChannel();
        m_client->startSession();
        QTRY_COMPARE(m_client->sessionState(), Gateway::SessionState::Active);
        QTRY_COMPARE(m_server->sessionState(), Gateway::SessionState::Active);
    }

private slots:
    void init();
    void cleanup();

    void handshake_bothEndsReachActive();
    void clientRequest_serverReplies();
    void serverInitiatedRequest_clientReplies();
    void fireAndForget_pushDeliveredToPeer();
    void retry_recoversFromLostRequestFrames();
    void keepAlive_bothEndsStayActive();
};

// Fresh pair of gateways + a cross-connected link before each test.
void TestIntegration::init()
{
    m_client = std::make_unique<Gateway>();
    m_server = std::make_unique<Gateway>();

    auto clientT = std::make_unique<LinkedTransport>();
    auto serverT = std::make_unique<LinkedTransport>();
    clientT->setName("client");
    serverT->setName("server");
    clientT->setPeer(serverT.get());
    serverT->setPeer(clientT.get());

    m_clientLink = equip(*m_client, std::move(clientT));
    m_serverLink = equip(*m_server, std::move(serverT));
}

void TestIntegration::cleanup()
{
    m_client.reset();
    m_server.reset();
    m_clientLink = nullptr;
    m_serverLink = nullptr;
}

// The SessionStart/SessionStartAck handshake completes across the wire:
// the server observes sessionStartReceived and both ends become Active.
void TestIntegration::handshake_bothEndsReachActive()
{
    QSignalSpy serverStart(m_server.get(), &Gateway::sessionStartReceived);

    m_client->enableChannel();
    m_server->enableChannel();
    m_client->startSession();

    QTRY_COMPARE(m_client->sessionState(), Gateway::SessionState::Active);
    QTRY_COMPARE(m_server->sessionState(), Gateway::SessionState::Active);
    QCOMPARE(serverStart.count(), 1);
}

// Client -> server: a request is delivered as requestReceived on the server,
// whose reply() round-trips back and resolves the client's GatewayRequest.
void TestIntegration::clientRequest_serverReplies()
{
    // Server answers every incoming request with "ACK:" + payload.
    connect(m_server.get(), &Gateway::requestReceived, m_server.get(),
            [this](quint32 corrId, const QByteArray &payload) {
                m_server->reply(corrId, QByteArray("ACK:") + payload);
            });

    establishSession();

    auto *req = m_client->sendRequest(QByteArray("PING"));
    QSignalSpy ok(req, &GatewayRequest::succeeded);

    QTRY_COMPARE(ok.count(), 1);
    QCOMPARE(ok.first().at(0).toByteArray(), QByteArray("ACK:PING"));
    QCOMPARE(req->status(), GatewayRequest::Status::Succeeded);
}

// Server -> client: the gateway is symmetric, so the side that accepted the
// session can also be the requester. The client answers; the server's
// request resolves. (This is the "server starts an operation" pattern.)
void TestIntegration::serverInitiatedRequest_clientReplies()
{
    connect(m_client.get(), &Gateway::requestReceived, m_client.get(),
            [this](quint32 corrId, const QByteArray &payload) {
                m_client->reply(corrId, QByteArray("CLIENT_ACK:") + payload);
            });

    establishSession();

    auto *req = m_server->sendRequest(QByteArray("DO_WORK"));
    QSignalSpy ok(req, &GatewayRequest::succeeded);

    QTRY_COMPARE(ok.count(), 1);
    QCOMPARE(ok.first().at(0).toByteArray(), QByteArray("CLIENT_ACK:DO_WORK"));
}

// Fire-and-forget: send() on one side surfaces as dataReceived on the other,
// with no correlation and no reply.
void TestIntegration::fireAndForget_pushDeliveredToPeer()
{
    QSignalSpy pushed(m_server.get(), &Gateway::dataReceived);

    establishSession();

    QVERIFY(m_client->send(QByteArray("EVENT:online")));

    QTRY_COMPARE(pushed.count(), 1);
    QCOMPARE(pushed.first().at(0).toByteArray(), QByteArray("EVENT:online"));
}

// Retry recovery: the link drops the first two request attempts; the
// client's RetryPolicy resends until one gets through and the peer replies.
void TestIntegration::retry_recoversFromLostRequestFrames()
{
    connect(m_server.get(), &Gateway::requestReceived, m_server.get(),
            [this](quint32 corrId, const QByteArray &payload) {
                m_server->reply(corrId, QByteArray("OK:") + payload);
            });

    establishSession();

    // Drop the next two frames the client puts on the wire. With keep-alive
    // off and nothing else in flight, those are the first two request attempts.
    m_clientLink->dropNextSends(2);

    Gateway::RetryPolicy policy;
    policy.maxRetries    = 5;
    policy.timeout       = std::chrono::milliseconds(60);
    policy.backoffFactor = 1.0;   // flat timeout — keeps the test quick and predictable
    policy.maxTimeout    = std::chrono::milliseconds(200);

    auto *req = m_client->sendRequest(QByteArray("IMPORTANT"), policy);
    QSignalSpy ok(req, &GatewayRequest::succeeded);
    QSignalSpy retry(req, &GatewayRequest::retrying);

    QTRY_COMPARE_WITH_TIMEOUT(ok.count(), 1, 3000);
    QCOMPARE(ok.first().at(0).toByteArray(), QByteArray("OK:IMPORTANT"));
    QVERIFY(retry.count() >= 2);          // at least the two dropped attempts were retried
    QVERIFY(req->attempts() >= 3);        // 2 lost + 1 that got through
}

// Mutual keep-alive: with the responder half implemented, each gateway answers
// the peer's KeepAliveReq with a KeepAliveReply. Over several heartbeat
// intervals neither side trips to Suspended, and both record received pongs.
void TestIntegration::keepAlive_bothEndsStayActive()
{
    Gateway::KeepAliveConfig ka;
    ka.enabled   = true;
    ka.interval  = std::chrono::milliseconds(30);
    ka.maxMissed = 3;
    m_client->setKeepAliveConfig(ka);
    m_server->setKeepAliveConfig(ka);

    establishSession();

    // Wait well past maxMissed * interval (~90 ms) so an unanswered heartbeat
    // would have forced Suspended by now.
    QTest::qWait(300);

    QCOMPARE(m_client->sessionState(), Gateway::SessionState::Active);
    QCOMPARE(m_server->sessionState(), Gateway::SessionState::Active);
    QVERIFY(m_client->stats().keepAlivesReceived > 0);
    QVERIFY(m_server->stats().keepAlivesReceived > 0);
}

QTEST_MAIN(TestIntegration)
#include "tst_Integration.moc"
