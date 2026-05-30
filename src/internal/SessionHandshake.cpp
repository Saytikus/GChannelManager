#include "SessionHandshake.h"

#include <QTimer>

#include "TimerMs.h"

namespace gcm::internal {

SessionHandshake::SessionHandshake(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SessionHandshake::onTimeout);
}

SessionHandshake::~SessionHandshake() = default;

void SessionHandshake::setTimeout(std::chrono::milliseconds timeout)
{
    m_timeout = timeout;
    if (m_timer->isActive()) {
        m_timer->stop();
        if (timeout.count() > 0)
            m_timer->start(timerMs(timeout));
    }
}

bool SessionHandshake::isAwaitingAck() const
{
    return m_timer->isActive();
}

qint64 SessionHandshake::initiate()
{
    if (!m_codec || !m_transport || !m_transport->isOpen())
        return -1;
    const qint64 bytes = sendFrame(m_codec->encodeSessionStart());
    if (bytes >= 0 && m_timeout.count() > 0)
        m_timer->start(timerMs(m_timeout));
    return bytes;
}

qint64 SessionHandshake::sendAck()
{
    if (!m_codec || !m_transport || !m_transport->isOpen())
        return -1;
    return sendFrame(m_codec->encodeSessionStartAck());
}

qint64 SessionHandshake::terminate()
{
    m_timer->stop();
    if (!m_codec || !m_transport || !m_transport->isOpen())
        return -1;
    return sendFrame(m_codec->encodeSessionStop());
}

void SessionHandshake::cancelTimeout()
{
    m_timer->stop();
}

void SessionHandshake::onTimeout()
{
    emit timedOut();
}

qint64 SessionHandshake::sendFrame(const QByteArray &frame)
{
    return m_transport->send(frame);
}

} // namespace gcm::internal
