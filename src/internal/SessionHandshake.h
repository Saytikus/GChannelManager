#pragma once

#include <QObject>
#include <chrono>

#include <GChannelManager/IMessageCodec.h>
#include <GChannelManager/ITransport.h>

class QTimer;

namespace gcm::internal {

// =====================================================================
//  Handshake начала и завершения сессии: SessionStart / SessionStartAck /
//  SessionStop, плюс таймер ожидания SessionStartAck. Состоянием сессии
//  не управляет — это работа Gateway, который слушает timedOut() и
//  принимает решение по результату.
// =====================================================================
class SessionHandshake : public QObject
{
    Q_OBJECT
public:
    explicit SessionHandshake(QObject *parent = nullptr);
    ~SessionHandshake() override;

    void setCodec(IMessageCodec *codec)      { m_codec = codec; }
    void setTransport(ITransport *transport) { m_transport = transport; }

    // Таймаут ожидания SessionStartAck. 0 — ждать бесконечно.
    void setTimeout(std::chrono::milliseconds timeout);
    [[nodiscard]] std::chrono::milliseconds timeout() const { return m_timeout; }

    // Отправляет SessionStart и стартует ack-таймер. Возвращает количество
    // отправленных байт (≥ 0) или -1, если кодек/транспорт не готовы.
    qint64 initiate();

    // Отправляет SessionStartAck. Используется при входящем SessionStart от узла.
    qint64 sendAck();

    // Отправляет SessionStop. Останавливает ack-таймер на всякий случай.
    qint64 terminate();

    // Остановить ack-таймер вручную (когда сессия установлена иначе).
    void cancelTimeout();

    [[nodiscard]] bool isAwaitingAck() const;

signals:
    void timedOut();        // SessionStartAck не пришёл в срок

private slots:
    void onTimeout();

private:
    qint64 sendFrame(const QByteArray &frame);

    IMessageCodec *m_codec     = nullptr;
    ITransport    *m_transport = nullptr;
    QTimer        *m_timer     = nullptr;
    std::chrono::milliseconds m_timeout{5000};
};

} // namespace gcm::internal
