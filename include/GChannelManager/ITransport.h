#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "GChannelManager_global.h"

// =====================================================================
//  The transport contract.
//  Implementations (serial port, UDP, RUDP...) are written separately
//  and are NOT part of this file. The gateway only knows this interface.
//
//  The transport is asynchronous: send() merely queues data for sending,
//  while incoming bytes arrive through the bytesReceived() signal.
// =====================================================================
class GCHANNELMANAGER_EXPORT ITransport : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Closed,   // closed
        Opening,  // opening
        Open,     // ready to exchange
        Closing,  // closing
        Error     // error
    };
    Q_ENUM(State)

    explicit ITransport(QObject *parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    [[nodiscard]] virtual State   state() const = 0;
    [[nodiscard]] virtual QString name()  const = 0;   // for logs/diagnostics
    [[nodiscard]] bool isOpen() const { return state() == State::Open; }

public slots:
    // Open/close the communication channel. Asynchronous: the result comes via opened()/closed().
    virtual void open()  = 0;
    virtual void close() = 0;

    // Queue data for sending.
    // Returns the number of accepted bytes, or -1 on error.
    virtual qint64 send(const QByteArray &data) = 0;

signals:
    void stateChanged(ITransport::State state);
    void opened();
    void closed();
    void bytesReceived(const QByteArray &data);   // raw incoming bytes
    void errorOccurred(const QString &message);
};
