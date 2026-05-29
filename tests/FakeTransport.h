#pragma once

#include <QByteArray>
#include <QList>

#include <GChannelManager/ITransport.h>

// =====================================================================
//  In-memory transport for unit tests.
//  Records everything the Gateway sends, and lets the test "deliver"
//  arbitrary bytes back (simulating the peer's reply).
// =====================================================================
class FakeTransport : public ITransport
{
    Q_OBJECT
public:
    using ITransport::ITransport;

    State   state() const override { return m_state; }
    QString name()  const override { return QStringLiteral("fake"); }

    // --- inspection (for tests) ---
    [[nodiscard]] const QList<QByteArray> &sent() const { return m_sent; }
    void clearSent() { m_sent.clear(); }

    // --- hooks for tests ---
    void simulateReceive(const QByteArray &bytes) {
        if (m_state == State::Open)
            emit bytesReceived(bytes);
    }
    void simulateError(const QString &msg) { emit errorOccurred(msg); }

public slots:
    void open() override
    {
        m_state = State::Open;
        emit stateChanged(m_state);
        emit opened();
    }
    void close() override
    {
        m_state = State::Closed;
        emit stateChanged(m_state);
        emit closed();
    }
    qint64 send(const QByteArray &data) override
    {
        if (m_state != State::Open)
            return -1;
        m_sent.append(data);
        emit dataSent(data);
        return data.size();
    }

signals:
    void dataSent(const QByteArray &data);   // test-only signal

private:
    State              m_state = State::Closed;
    QList<QByteArray>  m_sent;
};
