#pragma once

#include <QObject>

#include <GChannelManager/IMessageCodec.h>
#include <GChannelManager/ITransport.h>
#include <GChannelManager/KeepAliveConfig.h>

class QTimer;

namespace gcm::internal {

// =====================================================================
//  Heartbeat-таймер + счётчик пропусков. Принимает решение "линия молчит
//  слишком долго" (missedExceeded) и "линия вернулась" (recovered), но
//  не управляет состоянием сессии — это работа Gateway.
// =====================================================================
class KeepAliveMonitor : public QObject
{
    Q_OBJECT
public:
    explicit KeepAliveMonitor(QObject *parent = nullptr);
    ~KeepAliveMonitor() override;

    void setCodec(IMessageCodec *codec)      { m_codec = codec; }
    void setTransport(ITransport *transport) { m_transport = transport; }

    // Применить новую конфигурацию. Если heartbeat уже идёт и interval
    // изменился — таймер переставляется на новое значение. enabled=false
    // останавливает таймер; enabled=true сам по себе НЕ запускает —
    // запуск делает Gateway::enterActiveState().
    void setConfig(const KeepAliveConfig &c);
    [[nodiscard]] KeepAliveConfig config() const { return m_config; }
    [[nodiscard]] bool isEnabled() const { return m_config.enabled; }
    [[nodiscard]] bool isRunning() const;

    // Запустить heartbeat: сбрасывает missed, стартует таймер с config.interval,
    // и сразу же отправляет первый kalive (если codec/transport готовы).
    void start();
    void stop();

    // Известить о приходе keep-alive ответа от узла. Сбрасывает missed
    // и сигналит recovered, если был период "пропуска".
    void noteReply();

signals:
    void bytesPushed(qint64 bytes);     // отправлен heartbeat-кадр
    void replyReceived();                // пришёл KeepAliveReply (для счётчиков)
    void missedExceeded();               // missed > maxMissed (первый раз подряд)
    void recovered();                    // первый ответ после missedExceeded

private slots:
    void onTick();

private:
    IMessageCodec  *m_codec     = nullptr;
    ITransport     *m_transport = nullptr;
    KeepAliveConfig m_config{};
    QTimer         *m_timer     = nullptr;
    qint32          m_missed    = 0;
    bool            m_exceeded  = false;   // зафиксирован ли missedExceeded
};

} // namespace gcm::internal
