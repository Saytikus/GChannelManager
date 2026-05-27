#pragma once

#include <QByteArray>
#include <QObject>

#include "GChannelManager_global.h"

class Gateway;

// =====================================================================
//  Дескриптор отправленного запроса (по духу — как QNetworkReply).
//
//  Возвращается из Gateway::sendRequest(). Подписывайтесь на его сигналы
//  сразу после получения указателя. Объект живёт до завершения и сам
//  вызывает deleteLater() после finished().
//
//  Создаётся только гейтвеем (конструктор приватный, Gateway — friend).
// =====================================================================
class GCHANNELMANAGER_EXPORT GatewayRequest : public QObject
{
    Q_OBJECT
    friend class Gateway;
public:
    enum class Status { Pending, Succeeded, Failed };
    Q_ENUM(Status)

    enum class Error {
        None,
        Timeout,          // исчерпаны повторы
        Cancelled,        // отменён пользователем
        ChannelDisabled,  // канал выключен / транспорт закрыт
        SessionInactive,  // сессия не запущена
        TransportError    // нет кодека / ошибка отправки
    };
    Q_ENUM(Error)

    [[nodiscard]] quint32          id()          const { return m_id; }
    [[nodiscard]] qint32           attempts()    const { return m_attempts; }    // сделано попыток
    [[nodiscard]] qint32           maxAttempts() const { return m_maxAttempts; } // 1 + maxRetries
    [[nodiscard]] Status           status()      const { return m_status; }
    [[nodiscard]] Error            error()       const { return m_error; }
    [[nodiscard]] bool             isFinished()  const { return m_status != Status::Pending; }
    [[nodiscard]] const QByteArray &payload()    const { return m_payload; }
    [[nodiscard]] const QByteArray &response()   const { return m_response; }

public slots:
    void cancel() { if (!isFinished()) emit cancelRequested(); }

signals:
    void succeeded(const QByteArray &response);
    void failed(GatewayRequest::Error error);
    void retrying(qint32 attempt);   // началась повторная отправка №attempt
    void finished();              // ровно один раз, после succeeded/failed

    // внутренний: гейтвей слушает его, чтобы обработать отмену
    void cancelRequested();

private:
    explicit GatewayRequest(QObject *parent = nullptr) : QObject(parent) {}

    quint32    m_id          = 0;
    QByteArray m_payload;
    QByteArray m_response;
    qint32     m_attempts    = 0;
    qint32     m_maxAttempts = 1;
    Status     m_status      = Status::Pending;
    Error      m_error       = Error::None;
};
