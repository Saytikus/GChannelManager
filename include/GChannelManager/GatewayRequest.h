#pragma once

#include <QByteArray>
#include <QObject>

#include "GChannelManager_global.h"

class Gateway;
namespace gcm::internal { class PendingRequests; }

// =====================================================================
//  Descriptor of a sent request (in spirit — like QNetworkReply).
//
//  Returned from Gateway::sendRequest(). Connect to its signals right
//  after receiving the pointer. The object lives until completion and
//  calls deleteLater() on itself after finished().
//
//  Created only by the gateway (private constructor, Gateway is a friend).
// =====================================================================
class GCHANNELMANAGER_EXPORT GatewayRequest : public QObject
{
    Q_OBJECT
    friend class Gateway;
    friend class gcm::internal::PendingRequests;
public:
    enum class Status { Pending, Succeeded, Failed };
    Q_ENUM(Status)

    enum class Error {
        None,
        Timeout,          // retries exhausted
        Cancelled,        // cancelled by the user
        ChannelDisabled,  // channel off / transport closed
        SessionInactive,  // session not started
        TransportError    // no codec / send error
    };
    Q_ENUM(Error)

    [[nodiscard]] quint32          id()          const { return m_id; }
    [[nodiscard]] qint32           attempts()    const { return m_attempts; }    // attempts made
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
    void retrying(qint32 attempt);   // resend no. `attempt` has begun
    void finished();              // exactly once, after succeeded/failed

    // internal: the gateway listens to this to handle cancellation
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
