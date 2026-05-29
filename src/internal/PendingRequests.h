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
//  Manager of pending requests: correlation, retries, timeouts, and the
//  GatewayRequest lifecycle. An internal Gateway collaborator. Not part
//  of the public API.
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

    // Creates a GatewayRequest and registers it. The first attempt is scheduled
    // via QTimer::singleShot(0), so the caller has time to connect its signals.
    [[nodiscard]] GatewayRequest *enqueue(const QByteArray &payload, const RetryPolicy &policy);
    [[nodiscard]] GatewayRequest *enqueue(const QByteArray &payload) {
        return enqueue(payload, m_defaultPolicy);
    }

    // Creates an already-"failed" GatewayRequest — for cases where the
    // sendRequest preconditions are not met. failed/finished are emitted
    // via singleShot(0), so the caller can connect.
    [[nodiscard]] GatewayRequest *createPreflightFailed(GatewayRequest::Error err);

    // If a pending request with this corrId is found — complete it with success and return true.
    bool tryCompleteSuccess(quint32 corrId, const QByteArray &response);

    // Fail all pending requests with the given error (e.g. the session ended).
    void failAll(GatewayRequest::Error err);

    [[nodiscard]] bool isEmpty() const { return m_pending.isEmpty(); }

signals:
    void bytesPushed(qint64 bytes);            // data sent to the transport
    void retryStarted();                       // a retry attempt has begun
    void requestSucceeded();                   // successful completion
    void requestFailed();                      // final failure (any Error)

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
