#pragma once

#include <QObject>

#include <GChannelManager/IMessageCodec.h>
#include <GChannelManager/ITransport.h>
#include <GChannelManager/KeepAliveConfig.h>

class QTimer;

namespace gcm::internal {

// =====================================================================
//  Heartbeat timer + missed counter. Decides "the link has been silent
//  too long" (missedExceeded) and "the link is back" (recovered), but
//  does not manage session state — that is the Gateway's job.
// =====================================================================
class KeepAliveMonitor : public QObject
{
    Q_OBJECT
public:
    explicit KeepAliveMonitor(QObject *parent = nullptr);
    ~KeepAliveMonitor() override;

    void setCodec(IMessageCodec *codec)      { m_codec = codec; }
    void setTransport(ITransport *transport) { m_transport = transport; }

    // Apply a new configuration. If the heartbeat is already running and the
    // interval changed — the timer is rescheduled to the new value. enabled=false
    // stops the timer; enabled=true by itself does NOT start it —
    // starting is done by Gateway::enterActiveState().
    void setConfig(const KeepAliveConfig &c);
    [[nodiscard]] KeepAliveConfig config() const { return m_config; }
    [[nodiscard]] bool isEnabled() const { return m_config.enabled; }
    [[nodiscard]] bool isRunning() const;

    // Start the heartbeat: resets missed, starts the timer with config.interval,
    // and immediately sends the first kalive (if codec/transport are ready).
    void start();
    void stop();

    // Notify of an incoming keep-alive reply from the peer. Resets missed
    // and signals recovered if there was a "missed" period.
    void noteReply();

    // Answer an incoming keep-alive request from the peer with a reply frame.
    // Independent of the heartbeat timer / missed counter. Returns the number
    // of bytes sent (>= 0), or -1 if the codec/transport are not ready.
    qint64 answerPing();

signals:
    void bytesPushed(qint64 bytes);     // a heartbeat frame was sent
    void replyReceived();                // a KeepAliveReply arrived (for counters)
    void missedExceeded();               // missed > maxMissed (first time in a row)
    void recovered();                    // first reply after missedExceeded

private slots:
    void onTick();

private:
    IMessageCodec  *m_codec     = nullptr;
    ITransport     *m_transport = nullptr;
    KeepAliveConfig m_config{};
    QTimer         *m_timer     = nullptr;
    qint32          m_missed    = 0;
    bool            m_exceeded  = false;   // whether missedExceeded was latched
};

} // namespace gcm::internal
