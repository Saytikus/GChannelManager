#include "KeepAliveMonitor.h"

#include <QTimer>

namespace gcm::internal {

KeepAliveMonitor::KeepAliveMonitor(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &KeepAliveMonitor::onTick);
}

KeepAliveMonitor::~KeepAliveMonitor() = default;

bool KeepAliveMonitor::isRunning() const
{
    return m_timer->isActive();
}

void KeepAliveMonitor::setConfig(const KeepAliveConfig &c)
{
    const bool wasEnabled  = m_config.enabled;
    const auto oldInterval = m_config.interval;
    m_config = c;

    if (!c.enabled) {
        m_timer->stop();
        m_missed   = 0;
        m_exceeded = false;
        return;
    }

    if (wasEnabled && c.enabled && oldInterval != c.interval && m_timer->isActive()) {
        m_timer->setInterval(qint32(c.interval.count()));
    }
}

void KeepAliveMonitor::start()
{
    if (!m_config.enabled)
        return;
    m_missed   = 0;
    m_exceeded = false;
    m_timer->start(qint32(m_config.interval.count()));
    onTick();   // first heartbeat — immediately
}

void KeepAliveMonitor::stop()
{
    m_timer->stop();
    m_missed   = 0;
    m_exceeded = false;
}

void KeepAliveMonitor::onTick()
{
    if (!m_config.enabled)
        return;
    if (!m_codec || !m_transport || !m_transport->isOpen())
        return;

    const QByteArray frame = m_codec->encodeKeepAlive();
    const qint64 bytes = m_transport->send(frame);
    if (bytes >= 0)
        emit bytesPushed(bytes);

    ++m_missed;
    if (m_missed > m_config.maxMissed && !m_exceeded) {
        m_exceeded = true;
        emit missedExceeded();
    }
}

void KeepAliveMonitor::noteReply()
{
    m_missed = 0;
    emit replyReceived();
    if (m_exceeded) {
        m_exceeded = false;
        emit recovered();
    }
}

} // namespace gcm::internal
