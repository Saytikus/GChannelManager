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
//  Gateway — coordinator of the protocol layer.
//
//  It holds no correlation / heartbeat / handshake logic itself — it
//  delegates that to three collaborators (PendingRequests, KeepAliveMonitor,
//  SessionHandshake). Gateway is responsible only for:
//    * installing the transport/codec and proxying their signals,
//    * the state machines (ChannelState and SessionState),
//    * coordinating handling of incoming DecodedMessage::Type,
//    * fire-and-forget send and reply to incoming requests (+ idempotency cache),
//    * collecting and publishing statistics (GatewayStats).
//
//  Threading: the Gateway is single-threaded. It calls ITransport::send()
//  directly and drives QTimers, so the transport must live in the Gateway's
//  thread (the thread that owns this object and runs its event loop). To use a
//  transport on a worker thread, move the whole Gateway there, or front it with
//  a thread-safe transport that marshals send()/bytesReceived() across threads.
// =====================================================================
class GCHANNELMANAGER_EXPORT Gateway : public QObject
{
    Q_OBJECT
public:
    enum class ChannelState { Disabled, Enabled };
    Q_ENUM(ChannelState)

    enum class SessionState {
        Idle,          // no session
        Establishing,  // SessionStart sent, waiting for SessionStartAck
        Active,        // session established
        Suspended,     // link temporarily lost (RUDP mode); requests are still
                       // sent and in-flight ones keep retrying — they may time out
        Stopping       // stopping
    };
    Q_ENUM(SessionState)

    // Keep the familiar names `Gateway::RetryPolicy` / `Gateway::KeepAliveConfig`.
    using RetryPolicy      = ::RetryPolicy;
    using KeepAliveConfig  = ::KeepAliveConfig;
    using ReplyCacheConfig = ::ReplyCacheConfig;

    explicit Gateway(QObject *parent = nullptr);
    ~Gateway() override;

    // ---- installing transport and codec ----
    void setTransport(std::unique_ptr<ITransport> transport);   // the gateway takes ownership
    [[nodiscard]] ITransport *transport() const { return m_transport.get(); }

    void setCodec(std::unique_ptr<IMessageCodec> codec);
    [[nodiscard]] IMessageCodec *codec() const { return m_codec.get(); }

    // ---- configuration ----
    void setDefaultRetryPolicy(const RetryPolicy &p);
    [[nodiscard]] RetryPolicy defaultRetryPolicy() const;

    void setKeepAliveConfig(const KeepAliveConfig &k);
    [[nodiscard]] KeepAliveConfig keepAliveConfig() const;
    [[nodiscard]] bool isKeepAliveEnabled() const;

    // SessionStartAck wait timeout (0 — no timeout, wait forever).
    void setSessionStartTimeout(std::chrono::milliseconds timeout);
    [[nodiscard]] std::chrono::milliseconds sessionStartTimeout() const;

    // Cache of replies to incoming requests — for resends from the peer.
    void setReplyCacheConfig(const ReplyCacheConfig &c);
    [[nodiscard]] ReplyCacheConfig replyCacheConfig() const { return m_replyCacheConfig; }
    [[nodiscard]] bool isReplyCacheEnabled() const { return m_replyCacheConfig.enabled; }
    void clearReplyCache();

    // ---- states ----
    [[nodiscard]] ChannelState channelState()  const { return m_channel; }
    [[nodiscard]] SessionState sessionState()  const { return m_session; }
    [[nodiscard]] bool isChannelEnabled()      const { return m_channel == ChannelState::Enabled; }
    [[nodiscard]] bool isSessionActive()       const { return m_session == SessionState::Active; }

    // ---- statistics ----
    [[nodiscard]] GatewayStats stats() const { return m_stats; }
    void setStatsInterval(std::chrono::milliseconds interval);
    [[nodiscard]] std::chrono::milliseconds statsInterval() const { return m_statsInterval; }

public slots:
    // channel
    void enableChannel();
    void disableChannel();

    // session
    void startSession();
    void stopSession();

    // keep-alive on/off in a running session
    void setKeepAliveEnabled(bool enabled);

    // reply cache on/off in a running session
    void setReplyCacheEnabled(bool enabled);

    // reply to an incoming request (see requestReceived)
    bool reply(quint32 correlationId, const QByteArray &response);

    // reset all statistics counters to 0
    void resetStats();

    // send WITHOUT awaiting a reply (fire-and-forget)
    bool send(const QByteArray &payload);

    // send and await a reply
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
    // state transitions
    void setChannelState(ChannelState s);
    void setSessionState(SessionState s);
    void enterActiveState();   // common path for "ack received" / "peer opened a session"

    // transport handlers
    void onTransportOpened();
    void onTransportClosed();
    void onTransportBytes(const QByteArray &bytes);
    void onTransportError(const QString &msg);

    // propagate transport/codec to the collaborators
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
    std::chrono::milliseconds m_statsInterval{0};   // 0 = periodic emit disabled

    ReplyCacheConfig          m_replyCacheConfig{};
    QCache<quint32, QByteArray> m_replyCache{m_replyCacheConfig.maxEntries};
};
