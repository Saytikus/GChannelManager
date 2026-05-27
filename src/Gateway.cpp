#include "GChannelManager/Gateway.h"

#include <QTimer>
#include <cmath>

Gateway::Gateway(QObject *parent)
    : QObject(parent)
{
    m_keepAliveTimer = new QTimer(this);
    m_keepAliveTimer->setSingleShot(false);
    connect(m_keepAliveTimer, &QTimer::timeout, this, &Gateway::onKeepAliveTick);

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
}

// ---------------------------------------------------------------------
//  Keep-alive: конфигурация с применением "на лету"
// ---------------------------------------------------------------------
void Gateway::setKeepAliveConfig(const KeepAliveConfig &k)
{
    const bool wasEnabled  = m_keepAlive.enabled;
    const auto oldInterval = m_keepAlive.interval;
    m_keepAlive = k;

    const bool sessionRunning = (m_session != SessionState::Idle &&
                                 m_session != SessionState::Stopping);

    if (sessionRunning) {
        if (!wasEnabled && k.enabled) {
            applyKeepAliveStart();
        } else if (wasEnabled && !k.enabled) {
            applyKeepAliveStop();
        } else if (k.enabled && oldInterval != k.interval && m_keepAliveTimer->isActive()) {
            m_keepAliveTimer->setInterval(qint32(k.interval.count()));
        }
    }

    if (wasEnabled != k.enabled)
        emit keepAliveEnabledChanged(k.enabled);
}

void Gateway::setKeepAliveEnabled(bool enabled)
{
    if (m_keepAlive.enabled == enabled)
        return;
    KeepAliveConfig k = m_keepAlive;
    k.enabled = enabled;
    setKeepAliveConfig(k);
}

void Gateway::applyKeepAliveStart()
{
    if (!m_codec)
        return;
    m_missedKeepAlives = 0;
    m_keepAliveTimer->start(qint32(m_keepAlive.interval.count()));
    onKeepAliveTick();   // сразу шлём первый heartbeat
}

void Gateway::applyKeepAliveStop()
{
    m_keepAliveTimer->stop();
    m_missedKeepAlives = 0;
    // без heartbeat нечем выявить разрыв — считаем линк живым
    if (m_session == SessionState::Establishing || m_session == SessionState::Suspended)
        setSessionState(SessionState::Active);
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
    m_transport->open();   // подтверждение придёт в onTransportOpened()
}

void Gateway::disableChannel()
{
    if (m_session != SessionState::Idle)
        stopSession();
    if (m_transport)
        m_transport->close();   // подтверждение — в onTransportClosed()
    else
        setChannelState(ChannelState::Disabled);
}

void Gateway::onTransportOpened()
{
    setChannelState(ChannelState::Enabled);
}

void Gateway::onTransportClosed()
{
    m_keepAliveTimer->stop();
    failAllPending(GatewayRequest::Error::ChannelDisabled);
    setSessionState(SessionState::Idle);
    setChannelState(ChannelState::Disabled);
}

void Gateway::onTransportError(const QString &msg)
{
    emit errorOccurred(QStringLiteral("transport: ") + msg);
}

// ---------------------------------------------------------------------
//  Сессия + keep-alive (RUDP-подобная устойчивость к разрывам)
// ---------------------------------------------------------------------
void Gateway::startSession()
{
    if (!isChannelEnabled()) {
        emit errorOccurred(QStringLiteral("startSession: канал не включён"));
        return;
    }
    if (m_session == SessionState::Active || m_session == SessionState::Establishing)
        return;

    if (m_codec)
        m_codec->reset();
    m_missedKeepAlives = 0;

    if (m_keepAlive.enabled && m_codec) {
        setSessionState(SessionState::Establishing);
        m_keepAliveTimer->start(qint32(m_keepAlive.interval.count()));
        onKeepAliveTick();             // сразу шлём первый keep-alive
    } else {
        // без keep-alive считаем сессию активной немедленно
        setSessionState(SessionState::Active);
    }
}

void Gateway::stopSession()
{
    if (m_session == SessionState::Idle)
        return;
    setSessionState(SessionState::Stopping);
    m_keepAliveTimer->stop();
    failAllPending(GatewayRequest::Error::SessionInactive);
    setSessionState(SessionState::Idle);
}

void Gateway::onKeepAliveTick()
{
    if (m_session == SessionState::Idle || m_session == SessionState::Stopping)
        return;
    if (!m_keepAlive.enabled)
        return;
    if (!m_codec || !m_transport || !m_transport->isOpen())
        return;

    const QByteArray frame = m_codec->encodeKeepAlive();
    m_transport->send(frame);
    m_stats.sentBytes      += quint64(frame.size());
    m_stats.keepAlivesSent += 1;
    ++m_missedKeepAlives;

    if (m_missedKeepAlives > m_keepAlive.maxMissed) {
        if (m_session == SessionState::Active || m_session == SessionState::Establishing) {
            m_stats.suspensions += 1;
            setSessionState(SessionState::Suspended);  // линк пропал — ждём восстановления
        }
    }
}

void Gateway::onKeepAliveReply()
{
    m_stats.keepAlivesReceived += 1;
    m_missedKeepAlives = 0;
    if (m_session == SessionState::Establishing || m_session == SessionState::Suspended)
        setSessionState(SessionState::Active);
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
            if (m_pending.contains(msg.correlationId)) {
                completeSuccess(msg.correlationId, msg.payload);
            } else {
                m_stats.droppedReplies += 1;
                emit dataReceived(msg.payload);   // ответ без ожидающего запроса
            }
            break;
        case DecodedMessage::Type::Request:
            m_stats.incomingRequests += 1;
            if (m_replyCacheConfig.enabled && m_transport && m_transport->isOpen()) {
                if (const QByteArray *cached = m_replyCache.object(msg.correlationId)) {
                    // узел повторил уже отвеченный запрос — отправим сохранённый ответ,
                    // requestReceived НЕ эмитим, чтобы команду не выполнили повторно.
                    m_transport->send(*cached);
                    m_stats.sentBytes          += quint64(cached->size());
                    m_stats.cachedRepliesResent += 1;
                    break;
                }
            }
            emit requestReceived(msg.correlationId, msg.payload);
            break;
        case DecodedMessage::Type::KeepAlive:
            onKeepAliveReply();
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
//  Ответ на входящий запрос + кэш ответов
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
        m_replyCache.clear();   // disabled → освобождаем

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
//  Fire-and-forget отправка (без корреляции, без повторов)
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
//  Отправка с ожиданием ответа + повторы
// ---------------------------------------------------------------------
GatewayRequest *Gateway::sendRequest(const QByteArray &payload)
{
    return sendRequest(payload, m_defaultRetry);
}

GatewayRequest *Gateway::sendRequest(const QByteArray &payload, const RetryPolicy &policy)
{
    auto *req = new GatewayRequest(this);
    req->m_id          = nextId();
    req->m_payload     = payload;
    req->m_maxAttempts = 1 + policy.maxRetries;

    // проверка предусловий
    GatewayRequest::Error pre = GatewayRequest::Error::None;
    if (!m_codec)
        pre = GatewayRequest::Error::TransportError;
    else if (!isChannelEnabled())
        pre = GatewayRequest::Error::ChannelDisabled;
    else if (m_session == SessionState::Idle || m_session == SessionState::Stopping)
        pre = GatewayRequest::Error::SessionInactive;

    if (pre != GatewayRequest::Error::None) {
        failLater(req, pre);   // дать вызывающему подписаться на сигналы
        return req;
    }

    Pending p;
    p.policy = policy;
    p.frame  = m_codec->encodeRequest(req->m_id, payload);
    p.req    = req;
    p.timer  = new QTimer(this);
    p.timer->setSingleShot(true);

    const quint32 id = req->m_id;
    connect(p.timer, &QTimer::timeout, this, [this, id] { onAttemptTimeout(id); });
    connect(req, &GatewayRequest::cancelRequested, this,
            [this, id] { completeFailure(id, GatewayRequest::Error::Cancelled); });

    m_pending.insert(id, p);
    m_stats.requestsSent += 1;

    // первая отправка — в следующей итерации цикла событий,
    // чтобы вызывающий успел подключить сигналы к req
    QTimer::singleShot(0, this, [this, id] { startAttempt(id); });
    return req;
}

quint32 Gateway::nextId()
{
    quint32 id = m_nextId++;
    if (m_nextId == 0)
        m_nextId = 1;          // 0 зарезервирован (keep-alive)
    return id;
}

void Gateway::startAttempt(quint32 id)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    Pending &p = it.value();

    if (!m_transport || !m_transport->isOpen()) {
        completeFailure(id, GatewayRequest::Error::TransportError);
        return;
    }

    ++p.req->m_attempts;
    m_transport->send(p.frame);
    m_stats.sentBytes += quint64(p.frame.size());

    const auto ms = attemptTimeout(p.policy, p.req->m_attempts - 1);
    p.timer->start(qint32(ms.count()));
}

void Gateway::onAttemptTimeout(quint32 id)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    Pending &p = it.value();

    if (p.req->m_attempts >= 1 + p.policy.maxRetries) {
        completeFailure(id, GatewayRequest::Error::Timeout);
    } else {
        m_stats.retries += 1;
        emit p.req->retrying(p.req->m_attempts);
        startAttempt(id);      // повтор
    }
}

void Gateway::completeSuccess(quint32 id, const QByteArray &response)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    Pending p = it.value();
    m_pending.erase(it);

    if (p.timer) { p.timer->stop(); p.timer->deleteLater(); }

    p.req->m_response = response;
    p.req->m_status   = GatewayRequest::Status::Succeeded;
    m_stats.requestsSucceeded += 1;
    emit p.req->succeeded(response);
    emit p.req->finished();
    p.req->deleteLater();
}

void Gateway::completeFailure(quint32 id, GatewayRequest::Error err)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    Pending p = it.value();
    m_pending.erase(it);

    if (p.timer) { p.timer->stop(); p.timer->deleteLater(); }

    p.req->m_status = GatewayRequest::Status::Failed;
    p.req->m_error  = err;
    m_stats.requestsFailed += 1;
    emit p.req->failed(err);
    emit p.req->finished();
    p.req->deleteLater();
}

void Gateway::failLater(GatewayRequest *req, GatewayRequest::Error err)
{
    QTimer::singleShot(0, this, [this, req, err] {
        req->m_status = GatewayRequest::Status::Failed;
        req->m_error  = err;
        m_stats.requestsFailed += 1;
        emit req->failed(err);
        emit req->finished();
        req->deleteLater();
    });
}

void Gateway::failAllPending(GatewayRequest::Error err)
{
    const auto ids = m_pending.keys();
    for (quint32 id : ids)
        completeFailure(id, err);
}

std::chrono::milliseconds Gateway::attemptTimeout(const RetryPolicy &p, qint32 attempt) const
{
    double t = double(p.timeout.count()) * std::pow(p.backoffFactor, attempt);
    const double cap = double(p.maxTimeout.count());
    if (t > cap)
        t = cap;
    return std::chrono::milliseconds(static_cast<qint64>(std::llround(t)));
}

// ---------------------------------------------------------------------
//  Статистика
// ---------------------------------------------------------------------
void Gateway::setStatsInterval(std::chrono::milliseconds interval)
{
    m_statsInterval = interval;
    if (interval.count() > 0) {
        m_statsTimer->start(qint32(interval.count()));
    } else {
        m_statsTimer->stop();
    }
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
