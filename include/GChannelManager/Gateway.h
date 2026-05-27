#pragma once

#include <QHash>
#include <QObject>
#include <chrono>
#include <memory>

#include "GChannelManager_global.h"
#include "GatewayRequest.h"
#include "GatewayStats.h"
#include "IMessageCodec.h"
#include "ITransport.h"
#include "KeepAliveConfig.h"
#include "RetryPolicy.h"

class QTimer;

// =====================================================================
//  Гейтвей протокольного уровня.
//
//  Возможности:
//    * установка транспорта (сериал/UDP/...) и кодека;
//    * включение/выключение канала (open/close транспорта);
//    * старт/стоп сессии с keep-alive; устойчивость к разрывам линка
//      по типу RUDP: при пропаже keep-alive сессия уходит в Suspended,
//      при восстановлении — обратно в Active, канал при этом не рвётся;
//    * на лету включать/выключать keep-alive в работающей сессии
//      (setKeepAliveEnabled / setKeepAliveConfig);
//    * отправка с ожиданием ответа (sendRequest -> GatewayRequest);
//    * настраиваемые повторы (RetryPolicy: число, таймаут, backoff).
// =====================================================================
class GCHANNELMANAGER_EXPORT Gateway : public QObject
{
    Q_OBJECT
public:
    enum class ChannelState { Disabled, Enabled };
    Q_ENUM(ChannelState)

    enum class SessionState {
        Idle,          // сессии нет
        Establishing,  // устанавливается (ждём первый keep-alive)
        Active,        // активна
        Suspended,     // линк временно пропал (RUDP-режим), запросы ждут
        Stopping       // останавливается
    };
    Q_ENUM(SessionState)

    // Сохраняем привычные имена `Gateway::RetryPolicy` / `Gateway::KeepAliveConfig`.
    using RetryPolicy     = ::RetryPolicy;
    using KeepAliveConfig = ::KeepAliveConfig;

    explicit Gateway(QObject *parent = nullptr);
    ~Gateway() override;

    // ---- установка транспорта и кодека ----
    void setTransport(std::unique_ptr<ITransport> transport);   // гейтвей становится владельцем
    [[nodiscard]] ITransport *transport() const { return m_transport.get(); }

    void setCodec(std::unique_ptr<IMessageCodec> codec);
    [[nodiscard]] IMessageCodec *codec() const { return m_codec.get(); }

    // ---- конфигурация ----
    void setDefaultRetryPolicy(const RetryPolicy &p) { m_defaultRetry = p; }
    [[nodiscard]] RetryPolicy defaultRetryPolicy() const { return m_defaultRetry; }

    // Применяется на лету: если сессия запущена, изменения enabled/interval
    // подхватываются немедленно (heartbeat запускается/останавливается, сессия
    // переходит в Active/Establishing/Suspended при необходимости).
    void setKeepAliveConfig(const KeepAliveConfig &k);
    [[nodiscard]] KeepAliveConfig keepAliveConfig() const { return m_keepAlive; }
    [[nodiscard]] bool isKeepAliveEnabled() const { return m_keepAlive.enabled; }

    // ---- состояния ----
    [[nodiscard]] ChannelState channelState()  const { return m_channel; }
    [[nodiscard]] SessionState sessionState()  const { return m_session; }
    [[nodiscard]] bool isChannelEnabled()      const { return m_channel == ChannelState::Enabled; }
    [[nodiscard]] bool isSessionActive()       const { return m_session == SessionState::Active; }

    // ---- статистика ----
    // Снимок счётчиков "по требованию".
    [[nodiscard]] GatewayStats stats() const { return m_stats; }
    // Период автоэмита statsUpdated(). 0 — отключено (по умолчанию).
    void setStatsInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds statsInterval() const { return m_statsInterval; }

public slots:
    // канал
    void enableChannel();
    void disableChannel();

    // сессия
    void startSession();
    void stopSession();

    // keep-alive on/off в работающей сессии (как кнопка)
    void setKeepAliveEnabled(bool enabled);

    // сбросить все счётчики статистики в 0
    void resetStats();

    // отправка БЕЗ ожидания ответа (fire-and-forget); корреляция не используется,
    // повторов нет. Возвращает true, если кадр поставлен в очередь транспорта.
    bool send(const QByteArray &payload);

    // отправка с ожиданием ответа; подпишитесь на сигналы результата
    GatewayRequest *sendRequest(const QByteArray &payload);
    GatewayRequest *sendRequest(const QByteArray &payload, const RetryPolicy &policy);

signals:
    void channelStateChanged(Gateway::ChannelState state);
    void sessionStateChanged(Gateway::SessionState state);
    void keepAliveEnabledChanged(bool enabled);
    void errorOccurred(const QString &message);
    void dataReceived(const QByteArray &payload);   // некоррелированные данные (push)
    void statsUpdated(GatewayStats stats);          // периодический снимок счётчиков

private:
    struct Pending {
        GatewayRequest *req   = nullptr;
        RetryPolicy     policy;
        QByteArray      frame;            // готовый кадр (пере-отправляется при повторе)
        QTimer         *timer = nullptr;  // таймер ожидания ответа на попытку
    };

    // переходы состояний
    void setChannelState(ChannelState s);
    void setSessionState(SessionState s);

    // обработчики транспорта
    void onTransportOpened();
    void onTransportClosed();
    void onTransportBytes(const QByteArray &bytes);
    void onTransportError(const QString &msg);

    // keep-alive
    void onKeepAliveTick();
    void onKeepAliveReply();
    void applyKeepAliveStart();   // включить heartbeat в работающей сессии
    void applyKeepAliveStop();    // выключить heartbeat в работающей сессии

    // запросы / повторы
    quint32 nextId();
    void startAttempt(quint32 id);
    void onAttemptTimeout(quint32 id);
    void completeSuccess(quint32 id, const QByteArray &response);
    void completeFailure(quint32 id, GatewayRequest::Error err);
    void failLater(GatewayRequest *req, GatewayRequest::Error err);
    void failAllPending(GatewayRequest::Error err);

    [[nodiscard]] std::chrono::milliseconds attemptTimeout(const RetryPolicy &p, qint32 attempt) const;

    std::unique_ptr<ITransport>    m_transport;
    std::unique_ptr<IMessageCodec> m_codec;

    ChannelState m_channel = ChannelState::Disabled;
    SessionState m_session = SessionState::Idle;

    RetryPolicy     m_defaultRetry{};
    KeepAliveConfig m_keepAlive{};

    QTimer *m_keepAliveTimer = nullptr;
    qint32  m_missedKeepAlives = 0;

    quint32 m_nextId = 1;
    QHash<quint32, Pending> m_pending;

    GatewayStats              m_stats{};
    QTimer                   *m_statsTimer = nullptr;
    std::chrono::milliseconds m_statsInterval{0};   // 0 = периодический эмит выключен
};
