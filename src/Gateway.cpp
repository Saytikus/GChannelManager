#include "GChannelManager/Gateway.h"

#include <QTimer>

#include "internal/KeepAliveMonitor.h"
#include "internal/PendingRequests.h"
#include "internal/SessionHandshake.h"

using gcm::internal::KeepAliveMonitor;
using gcm::internal::PendingRequests;
using gcm::internal::SessionHandshake;

// ---------------------------------------------------------------------
//  Конструктор: создаём коллабораторов и связываем их сигналы со
//  счётчиками статистики Gateway. Все коллабораторы — QObject, владение
//  через std::unique_ptr (без Qt parent — чтобы избежать двойного владения).
// ---------------------------------------------------------------------
Gateway::Gateway(QObject *parent)
    : QObject(parent)
    , m_requests (std::make_unique<PendingRequests>())
    , m_keepAlive(std::make_unique<KeepAliveMonitor>())
    , m_handshake(std::make_unique<SessionHandshake>())
{
    // Статистика на основе сигналов коллабораторов.
    connect(m_requests.get(), &PendingRequests::bytesPushed,
            this, [this](qint64 b) { m_stats.sentBytes += quint64(b); });
    connect(m_requests.get(), &PendingRequests::retryStarted,
            this, [this] { m_stats.retries += 1; });
    connect(m_requests.get(), &PendingRequests::requestSucceeded,
            this, [this] { m_stats.requestsSucceeded += 1; });
    connect(m_requests.get(), &PendingRequests::requestFailed,
            this, [this] { m_stats.requestsFailed += 1; });

    connect(m_keepAlive.get(), &KeepAliveMonitor::bytesPushed,
            this, [this](qint64 b) {
                m_stats.sentBytes      += quint64(b);
                m_stats.keepAlivesSent += 1;
            });
    connect(m_keepAlive.get(), &KeepAliveMonitor::replyReceived,
            this, [this] { m_stats.keepAlivesReceived += 1; });
    connect(m_keepAlive.get(), &KeepAliveMonitor::missedExceeded,
            this, [this] {
                if (m_session == SessionState::Active) {
                    m_stats.suspensions += 1;
                    setSessionState(SessionState::Suspended);
                }
            });
    connect(m_keepAlive.get(), &KeepAliveMonitor::recovered,
            this, [this] {
                if (m_session == SessionState::Suspended)
                    setSessionState(SessionState::Active);
            });

    connect(m_handshake.get(), &SessionHandshake::timedOut,
            this, [this] {
                if (m_session != SessionState::Establishing)
                    return;
                m_stats.sessionStartTimeouts += 1;
                emit errorOccurred(QStringLiteral("startSession: SessionStartAck не получен в срок"));
                setSessionState(SessionState::Idle);
            });

    m_statsTimer = new QTimer(this);
    m_statsTimer->setSingleShot(false);
    connect(m_statsTimer, &QTimer::timeout, this, [this] { emit statsUpdated(m_stats); });
}

Gateway::~Gateway() = default;

// ---------------------------------------------------------------------
//  Установка транспорта / кодека
// ---------------------------------------------------------------------
void Gateway::setTransport(std::unique_ptr<ITransport> transport)
{
    if (m_transport) {
        disableChannel();
        disconnect(m_transport.get(), nullptr, this, nullptr);
        m_transport.reset();
    }

    m_transport = std::move(transport);
    propagateTransport();
    if (!m_transport)
        return;

    connect(m_transport.get(), &ITransport::opened,        this, &Gateway::onTransportOpened);
    connect(m_transport.get(), &ITransport::closed,        this, &Gateway::onTransportClosed);
    connect(m_transport.get(), &ITransport::bytesReceived, this, &Gateway::onTransportBytes);
    connect(m_transport.get(), &ITransport::errorOccurred, this, &Gateway::onTransportError);
}

void Gateway::setCodec(std::unique_ptr<IMessageCodec> codec)
{
    m_codec = std::move(codec);
    propagateCodec();
}

void Gateway::propagateTransport()
{
    ITransport *t = m_transport.get();
    m_requests ->setTransport(t);
    m_keepAlive->setTransport(t);
    m_handshake->setTransport(t);
}

void Gateway::propagateCodec()
{
    IMessageCodec *c = m_codec.get();
    m_requests ->setCodec(c);
    m_keepAlive->setCodec(c);
    m_handshake->setCodec(c);
}

// ---------------------------------------------------------------------
//  Конфигурация — проксируется коллабораторам
// ---------------------------------------------------------------------
void Gateway::setDefaultRetryPolicy(const RetryPolicy &p) { m_requests->setDefaultPolicy(p); }
Gateway::RetryPolicy Gateway::defaultRetryPolicy() const  { return m_requests->defaultPolicy(); }

Gateway::KeepAliveConfig Gateway::keepAliveConfig() const { return m_keepAlive->config(); }
bool Gateway::isKeepAliveEnabled() const                  { return m_keepAlive->isEnabled(); }

void Gateway::setSessionStartTimeout(std::chrono::milliseconds timeout)
{
    m_handshake->setTimeout(timeout);
}

std::chrono::milliseconds Gateway::sessionStartTimeout() const
{
    return m_handshake->timeout();
}

void Gateway::setKeepAliveConfig(const KeepAliveConfig &k)
{
    const bool wasEnabled = m_keepAlive->isEnabled();
    m_keepAlive->setConfig(k);

    if (m_session == SessionState::Active && !wasEnabled && k.enabled) {
        // включили heartbeat в работающей сессии
        m_keepAlive->start();
    } else if (wasEnabled && !k.enabled && m_session == SessionState::Suspended) {
        // выключили heartbeat в Suspended — нечем выявить разрыв, считаем линк живым
        setSessionState(SessionState::Active);
    }

    if (wasEnabled != k.enabled)
        emit keepAliveEnabledChanged(k.enabled);
}

void Gateway::setKeepAliveEnabled(bool enabled)
{
    if (m_keepAlive->isEnabled() == enabled)
        return;
    KeepAliveConfig c = m_keepAlive->config();
    c.enabled = enabled;
    setKeepAliveConfig(c);
}

// ---------------------------------------------------------------------
//  Канал
// ---------------------------------------------------------------------
void Gateway::enableChannel()
{
    if (!m_transport) {
        emit errorOccurred(QStringLiteral("enableChannel: транспорт не установлен"));
        return;
    }
    if (isChannelEnabled())
        return;
    m_transport->open();
}

void Gateway::disableChannel()
{
    if (m_session != SessionState::Idle)
        stopSession();
    if (m_transport)
        m_transport->close();
    else
        setChannelState(ChannelState::Disabled);
}

void Gateway::onTransportOpened()
{
    setChannelState(ChannelState::Enabled);
}

void Gateway::onTransportClosed()
{
    m_handshake->cancelTimeout();
    m_keepAlive->stop();
    m_requests->failAll(GatewayRequest::Error::ChannelDisabled);
    setSessionState(SessionState::Idle);
    setChannelState(ChannelState::Disabled);
}

void Gateway::onTransportError(const QString &msg)
{
    emit errorOccurred(QStringLiteral("transport: ") + msg);
}

// ---------------------------------------------------------------------
//  Сессия
// ---------------------------------------------------------------------
void Gateway::startSession()
{
    if (!isChannelEnabled()) {
        emit errorOccurred(QStringLiteral("startSession: канал не включён"));
        return;
    }
    if (m_session == SessionState::Active || m_session == SessionState::Establishing)
        return;
    if (!m_codec) {
        emit errorOccurred(QStringLiteral("startSession: кодек не установлен"));
        return;
    }

    m_codec->reset();

    setSessionState(SessionState::Establishing);
    const qint64 bytes = m_handshake->initiate();
    if (bytes >= 0) {
        m_stats.sentBytes         += quint64(bytes);
        m_stats.sessionStartsSent += 1;
    }
}

void Gateway::stopSession()
{
    if (m_session == SessionState::Idle)
        return;
    setSessionState(SessionState::Stopping);
    m_handshake->cancelTimeout();
    m_keepAlive->stop();

    const qint64 bytes = m_handshake->terminate();
    if (bytes >= 0) {
        m_stats.sentBytes        += quint64(bytes);
        m_stats.sessionStopsSent += 1;
    }
    m_requests->failAll(GatewayRequest::Error::SessionInactive);
    setSessionState(SessionState::Idle);
}

void Gateway::enterActiveState()
{
    m_handshake->cancelTimeout();
    setSessionState(SessionState::Active);
    if (m_keepAlive->isEnabled())
        m_keepAlive->start();
}

// ---------------------------------------------------------------------
//  Приём данных
// ---------------------------------------------------------------------
void Gateway::onTransportBytes(const QByteArray &bytes)
{
    m_stats.recvBytes += quint64(bytes.size());

    if (!m_codec)
        return;

    for (const auto &msg : m_codec->feed(bytes)) {
        switch (msg.type) {
        case DecodedMessage::Type::Reply:
            if (!m_requests->tryCompleteSuccess(msg.correlationId, msg.payload)) {
                m_stats.droppedReplies += 1;
                emit dataReceived(msg.payload);
            }
            break;
        case DecodedMessage::Type::Request:
            m_stats.incomingRequests += 1;
            if (m_replyCacheConfig.enabled && m_transport && m_transport->isOpen()) {
                if (const QByteArray *cached = m_replyCache.object(msg.correlationId)) {
                    m_transport->send(*cached);
                    m_stats.sentBytes          += quint64(cached->size());
                    m_stats.cachedRepliesResent += 1;
                    break;
                }
            }
            emit requestReceived(msg.correlationId, msg.payload);
            break;
        case DecodedMessage::Type::SessionStart:
            m_stats.sessionStartsReceived += 1;
            {
                const qint64 b = m_handshake->sendAck();
                if (b >= 0)
                    m_stats.sentBytes += quint64(b);
            }
            if (m_session == SessionState::Idle) {
                m_codec->reset();
                enterActiveState();
            }
            emit sessionStartReceived();
            break;
        case DecodedMessage::Type::SessionStartAck:
            if (m_session == SessionState::Establishing)
                enterActiveState();
            break;
        case DecodedMessage::Type::SessionStop:
            m_stats.sessionStopsReceived += 1;
            if (m_session != SessionState::Idle && m_session != SessionState::Stopping) {
                m_handshake->cancelTimeout();
                m_keepAlive->stop();
                m_requests->failAll(GatewayRequest::Error::SessionInactive);
                setSessionState(SessionState::Idle);
            }
            emit sessionStopReceived();
            break;
        case DecodedMessage::Type::KeepAlive:
            m_keepAlive->noteReply();
            break;
        case DecodedMessage::Type::Data:
            m_stats.dataReceived += 1;
            emit dataReceived(msg.payload);
            break;
        case DecodedMessage::Type::Unknown:
            break;
        }
    }
}

// ---------------------------------------------------------------------
//  reply (+ idempotency-кэш)
// ---------------------------------------------------------------------
bool Gateway::reply(quint32 correlationId, const QByteArray &response)
{
    if (!m_codec) {
        emit errorOccurred(QStringLiteral("reply: кодек не установлен"));
        return false;
    }
    if (!isChannelEnabled()) {
        emit errorOccurred(QStringLiteral("reply: канал не включён"));
        return false;
    }
    if (m_session == SessionState::Idle || m_session == SessionState::Stopping) {
        emit errorOccurred(QStringLiteral("reply: сессия не активна"));
        return false;
    }
    if (!m_transport || !m_transport->isOpen()) {
        emit errorOccurred(QStringLiteral("reply: транспорт закрыт"));
        return false;
    }
    const QByteArray frame = m_codec->encodeReply(correlationId, response);
    if (m_transport->send(frame) < 0)
        return false;

    m_stats.sentBytes += quint64(frame.size());
    if (m_replyCacheConfig.enabled)
        m_replyCache.insert(correlationId, new QByteArray(frame));
    return true;
}

void Gateway::setReplyCacheConfig(const ReplyCacheConfig &c)
{
    const bool wasEnabled = m_replyCacheConfig.enabled;
    const auto oldMax     = m_replyCacheConfig.maxEntries;
    m_replyCacheConfig    = c;

    if (oldMax != c.maxEntries)
        m_replyCache.setMaxCost(c.maxEntries);
    if (wasEnabled && !c.enabled)
        m_replyCache.clear();

    if (wasEnabled != c.enabled)
        emit replyCacheEnabledChanged(c.enabled);
}

void Gateway::setReplyCacheEnabled(bool enabled)
{
    if (m_replyCacheConfig.enabled == enabled)
        return;
    ReplyCacheConfig c = m_replyCacheConfig;
    c.enabled = enabled;
    setReplyCacheConfig(c);
}

void Gateway::clearReplyCache()
{
    m_replyCache.clear();
}

// ---------------------------------------------------------------------
//  Fire-and-forget
// ---------------------------------------------------------------------
bool Gateway::send(const QByteArray &payload)
{
    if (!m_codec) {
        emit errorOccurred(QStringLiteral("send: кодек не установлен"));
        return false;
    }
    if (!isChannelEnabled()) {
        emit errorOccurred(QStringLiteral("send: канал не включён"));
        return false;
    }
    if (m_session == SessionState::Idle || m_session == SessionState::Stopping) {
        emit errorOccurred(QStringLiteral("send: сессия не активна"));
        return false;
    }
    if (!m_transport || !m_transport->isOpen()) {
        emit errorOccurred(QStringLiteral("send: транспорт закрыт"));
        return false;
    }
    const QByteArray frame = m_codec->encodeData(payload);
    if (m_transport->send(frame) < 0)
        return false;
    m_stats.sentBytes         += quint64(frame.size());
    m_stats.fireAndForgetSent += 1;
    return true;
}

// ---------------------------------------------------------------------
//  Запрос с ожиданием ответа — делегация PendingRequests
// ---------------------------------------------------------------------
GatewayRequest *Gateway::sendRequest(const QByteArray &payload)
{
    return sendRequest(payload, m_requests->defaultPolicy());
}

GatewayRequest *Gateway::sendRequest(const QByteArray &payload, const RetryPolicy &policy)
{
    if (!m_codec)
        return m_requests->createPreflightFailed(GatewayRequest::Error::TransportError);
    if (!isChannelEnabled())
        return m_requests->createPreflightFailed(GatewayRequest::Error::ChannelDisabled);
    if (m_session == SessionState::Idle || m_session == SessionState::Stopping)
        return m_requests->createPreflightFailed(GatewayRequest::Error::SessionInactive);

    m_stats.requestsSent += 1;
    return m_requests->enqueue(payload, policy);
}

// ---------------------------------------------------------------------
//  Статистика
// ---------------------------------------------------------------------
void Gateway::setStatsInterval(std::chrono::milliseconds interval)
{
    m_statsInterval = interval;
    if (interval.count() > 0)
        m_statsTimer->start(qint32(interval.count()));
    else
        m_statsTimer->stop();
}

void Gateway::resetStats()
{
    m_stats = {};
}

// ---------------------------------------------------------------------
//  Переходы состояний
// ---------------------------------------------------------------------
void Gateway::setChannelState(ChannelState s)
{
    if (m_channel == s)
        return;
    m_channel = s;
    emit channelStateChanged(s);
}

void Gateway::setSessionState(SessionState s)
{
    if (m_session == s)
        return;
    m_session = s;
    emit sessionStateChanged(s);
}
