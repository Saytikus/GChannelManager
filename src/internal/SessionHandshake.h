#pragma once

#include <QObject>
#include <chrono>

#include <GChannelManager/IMessageCodec.h>
#include <GChannelManager/ITransport.h>

class QTimer;

namespace gcm::internal {

// =====================================================================
//  Session start/teardown handshake: SessionStart / SessionStartAck /
//  SessionStop, plus the SessionStartAck wait timer. Does not manage
//  session state — that is the Gateway's job, which listens to timedOut()
//  and decides what to do with the result.
// =====================================================================
class SessionHandshake : public QObject
{
    Q_OBJECT
public:
    explicit SessionHandshake(QObject *parent = nullptr);
    ~SessionHandshake() override;

    void setCodec(IMessageCodec *codec)      { m_codec = codec; }
    void setTransport(ITransport *transport) { m_transport = transport; }

    // SessionStartAck wait timeout. 0 — wait forever.
    void setTimeout(std::chrono::milliseconds timeout);
    [[nodiscard]] std::chrono::milliseconds timeout() const { return m_timeout; }

    // Sends SessionStart and starts the ack timer. Returns the number of
    // bytes sent (≥ 0), or -1 if the codec/transport are not ready.
    qint64 initiate();

    // Sends SessionStartAck. Used on an incoming SessionStart from the peer.
    qint64 sendAck();

    // Sends SessionStop. Stops the ack timer just in case.
    qint64 terminate();

    // Stop the ack timer manually (when the session is established otherwise).
    void cancelTimeout();

    [[nodiscard]] bool isAwaitingAck() const;

signals:
    void timedOut();        // SessionStartAck did not arrive in time

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
