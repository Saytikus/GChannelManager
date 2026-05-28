#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <chrono>
#include <optional>

#include <GChannelManager/GatewayRequest.h>
#include <GChannelManager/IMessageCodec.h>
#include <GChannelManager/ITransport.h>
#include <GChannelManager/RetryPolicy.h>

class QTimer;

namespace gcm::internal {

// =====================================================================
//  Менеджер pending-запросов: корреляция, ретраи, таймауты, lifecycle
//  GatewayRequest. Внутренний коллаборатор Gateway. Не часть публичного API.
// =====================================================================
class PendingRequests : public QObject
{
    Q_OBJECT
public:
    explicit PendingRequests(QObject *parent = nullptr);
    ~PendingRequests() override;

    void setCodec(IMessageCodec *codec)        { m_codec = codec; }
    void setTransport(ITransport *transport)   { m_transport = transport; }
    void setDefaultPolicy(const RetryPolicy &p) { m_defaultPolicy = p; }
    [[nodiscard]] const RetryPolicy &defaultPolicy() const { return m_defaultPolicy; }

    // Создаёт GatewayRequest и регистрирует его. Первая попытка планируется
    // через QTimer::singleShot(0), чтобы вызывающий успел подключить сигналы.
    [[nodiscard]] GatewayRequest *enqueue(const QByteArray &payload, const RetryPolicy &policy);
    [[nodiscard]] GatewayRequest *enqueue(const QByteArray &payload) {
        return enqueue(payload, m_defaultPolicy);
    }

    // Создаёт уже "проваленный" GatewayRequest — для случаев, когда
    // предусловия sendRequest не выполнены. failed/finished эмитятся
    // через singleShot(0), чтобы вызывающий мог подключиться.
    [[nodiscard]] GatewayRequest *createPreflightFailed(GatewayRequest::Error err);

    // Если найден pending с таким corrId — завершить успехом и вернуть true.
    bool tryCompleteSuccess(quint32 corrId, const QByteArray &response);

    // Провалить все pending с указанной ошибкой (например, сессия завершена).
    void failAll(GatewayRequest::Error err);

    [[nodiscard]] bool isEmpty() const { return m_pending.isEmpty(); }

signals:
    void bytesPushed(qint64 bytes);            // данные отправлены в транспорт
    void retryStarted();                       // началась повторная попытка
    void requestSucceeded();                   // успешное завершение
    void requestFailed();                      // финальный отказ (любая Error)

private:
    struct Pending {
        GatewayRequest *req   = nullptr;
        RetryPolicy     policy;
        QByteArray      frame;
        QTimer         *timer = nullptr;
    };

    quint32 nextId();
    void    startAttempt(quint32 id);
    void    onAttemptTimeout(quint32 id);
    void    complete(quint32 id, bool ok,
                     const QByteArray &response, GatewayRequest::Error err);
    [[nodiscard]] std::chrono::milliseconds
            attemptTimeout(const RetryPolicy &p, qint32 attempt) const;

    [[nodiscard]] std::optional<std::reference_wrapper<Pending>> find(quint32 id);

    IMessageCodec  *m_codec     = nullptr;
    ITransport     *m_transport = nullptr;
    RetryPolicy     m_defaultPolicy{};
    quint32         m_nextId    = 1;
    QHash<quint32, Pending> m_pending;
};

} // namespace gcm::internal
