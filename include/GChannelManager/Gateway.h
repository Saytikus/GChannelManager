#pragma once

#include <QByteArray>
#include <QCache>
#include <QObject>
#include <chrono>
#include <memory>

#include "GChannelManager_global.h"
#include "GatewayRequest.h"
#include "GatewayStats.h"
#include "IMessageCodec.h"
#include "ITransport.h"
#include "KeepAliveConfig.h"
#include "ReplyCacheConfig.h"
#include "RetryPolicy.h"

class QTimer;

namespace gcm::internal {
class PendingRequests;
class KeepAliveMonitor;
class SessionHandshake;
}

// =====================================================================
//  Gateway — координатор протокольного слоя.
//
//  Сам по себе не содержит логики корреляции / heartbeat / handshake —
//  делегирует это трём коллабораторам (PendingRequests, KeepAliveMonitor,
//  SessionHandshake). Gateway отвечает только за:
//    * установку транспорта/кодека и проксирование их сигналов,
//    * машины состояний (ChannelState и SessionState),
//    * координацию обработки входящего DecodedMessage::Type,
//    * fire-and-forget send и reply на входящие запросы (+ idempotency-кэш),
//    * сбор и публикацию статистики (GatewayStats).
// =====================================================================
class GCHANNELMANAGER_EXPORT Gateway : public QObject
{
    Q_OBJECT
public:
    enum class ChannelState { Disabled, Enabled };
    Q_ENUM(ChannelState)

    enum class SessionState {
        Idle,          // сессии нет
        Establishing,  // отправлен SessionStart, ждём SessionStartAck
        Active,        // сессия установлена
        Suspended,     // линк временно пропал (RUDP-режим), запросы ждут
        Stopping       // останавливается
    };
    Q_ENUM(SessionState)

    // Сохраняем привычные имена `Gateway::RetryPolicy` / `Gateway::KeepAliveConfig`.
    using RetryPolicy      = ::RetryPolicy;
    using KeepAliveConfig  = ::KeepAliveConfig;
    using ReplyCacheConfig = ::ReplyCacheConfig;

    explicit Gateway(QObject *parent = nullptr);
    ~Gateway() override;

    // ---- установка транспорта и кодека ----
    void setTransport(std::unique_ptr<ITransport> transport);   // гейтвей становится владельцем
    [[nodiscard]] ITransport *transport() const { return m_transport.get(); }

    void setCodec(std::unique_ptr<IMessageCodec> codec);
    [[nodiscard]] IMessageCodec *codec() const { return m_codec.get(); }

    // ---- конфигурация ----
    void setDefaultRetryPolicy(const RetryPolicy &p);
    [[nodiscard]] RetryPolicy defaultRetryPolicy() const;

    void setKeepAliveConfig(const KeepAliveConfig &k);
    [[nodiscard]] KeepAliveConfig keepAliveConfig() const;
    [[nodiscard]] bool isKeepAliveEnabled() const;

    // Таймаут ожидания SessionStartAck (0 — без таймаута, ждём бесконечно).
    void setSessionStartTimeout(std::chrono::milliseconds timeout);
    [[nodiscard]] std::chrono::milliseconds sessionStartTimeout() const;

    // Кэш ответов на входящие запросы — для повторов от узла.
    void setReplyCacheConfig(const ReplyCacheConfig &c);
    [[nodiscard]] ReplyCacheConfig replyCacheConfig() const { return m_replyCacheConfig; }
    [[nodiscard]] bool isReplyCacheEnabled() const { return m_replyCacheConfig.enabled; }
    void clearReplyCache();

    // ---- состояния ----
    [[nodiscard]] ChannelState channelState()  const { return m_channel; }
    [[nodiscard]] SessionState sessionState()  const { return m_session; }
    [[nodiscard]] bool isChannelEnabled()      const { return m_channel == ChannelState::Enabled; }
    [[nodiscard]] bool isSessionActive()       const { return m_session == SessionState::Active; }

    // ---- статистика ----
    [[nodiscard]] GatewayStats stats() const { return m_stats; }
    void setStatsInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds statsInterval() const { return m_statsInterval; }

public slots:
    // канал
    void enableChannel();
    void disableChannel();

    // сессия
    void startSession();
    void stopSession();

    // keep-alive on/off в работающей сессии
    void setKeepAliveEnabled(bool enabled);

    // кэш ответов on/off в работающей сессии
    void setReplyCacheEnabled(bool enabled);

    // ответ на входящий запрос (см. requestReceived)
    bool reply(quint32 correlationId, const QByteArray &response);

    // сбросить все счётчики статистики в 0
    void resetStats();

    // отправка БЕЗ ожидания ответа (fire-and-forget)
    bool send(const QByteArray &payload);

    // отправка с ожиданием ответа
    GatewayRequest *sendRequest(const QByteArray &payload);
    GatewayRequest *sendRequest(const QByteArray &payload, const RetryPolicy &policy);

signals:
    void channelStateChanged(Gateway::ChannelState state);
    void sessionStateChanged(Gateway::SessionState state);
    void keepAliveEnabledChanged(bool enabled);
    void replyCacheEnabledChanged(bool enabled);
    void errorOccurred(const QString &message);
    void dataReceived(const QByteArray &payload);
    void requestReceived(quint32 correlationId, const QByteArray &payload);
    void sessionStartReceived();
    void sessionStopReceived();
    void statsUpdated(GatewayStats stats);

private:
    // переходы состояний
    void setChannelState(ChannelState s);
    void setSessionState(SessionState s);
    void enterActiveState();   // общий путь "ack получен" / "peer открыл сессию"

    // обработчики транспорта
    void onTransportOpened();
    void onTransportClosed();
    void onTransportBytes(const QByteArray &bytes);
    void onTransportError(const QString &msg);

    // распределение прав на коллабораторов
    void propagateCodec();
    void propagateTransport();

    std::unique_ptr<ITransport>    m_transport;
    std::unique_ptr<IMessageCodec> m_codec;

    std::unique_ptr<gcm::internal::PendingRequests>  m_requests;
    std::unique_ptr<gcm::internal::KeepAliveMonitor> m_keepAlive;
    std::unique_ptr<gcm::internal::SessionHandshake> m_handshake;

    ChannelState m_channel = ChannelState::Disabled;
    SessionState m_session = SessionState::Idle;

    GatewayStats              m_stats{};
    QTimer                   *m_statsTimer = nullptr;
    std::chrono::milliseconds m_statsInterval{0};   // 0 = периодический эмит выключен

    ReplyCacheConfig          m_replyCacheConfig{};
    QCache<quint32, QByteArray> m_replyCache{m_replyCacheConfig.maxEntries};
};
