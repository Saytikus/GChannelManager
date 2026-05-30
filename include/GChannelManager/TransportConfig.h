#pragma once

#include <QString>
#include <chrono>

// =====================================================================
//  Transport configuration.
//  These structures are passed to a concrete transport's constructor:
//      auto t = std::make_unique<SerialTransport>(SerialConfig{...});
//  Here — data only, with no dependency on QtSerialPort, so that the
//  contract stays "light".
// =====================================================================
namespace transport {

struct SerialConfig {
    QString portName;                 // "COM3", "/dev/ttyUSB0"
    qint32  baudRate = 115200;
    qint32  dataBits = 8;

    enum class Parity      { None, Even, Odd, Space, Mark };
    enum class StopBits    { One, OneAndHalf, Two };
    enum class FlowControl { None, Hardware, Software };

    Parity      parity      = Parity::None;
    StopBits    stopBits    = StopBits::One;
    FlowControl flowControl = FlowControl::None;

    std::chrono::milliseconds writeTimeout{500};
};

struct UdpConfig {
    // Addresses as plain strings ("0.0.0.0", "192.168.1.10", "::1") so the
    // core library carries no QtNetwork dependency. A concrete UDP transport
    // parses them into QHostAddress on its own side.
    QString localAddress   = QStringLiteral("0.0.0.0");
    quint16 localPort      = 0;            // 0 — any free port
    QString remoteAddress;                 // destination peer address
    quint16 remotePort     = 0;
    bool    bindBeforeSend = true;         // bind the socket on open()
};

} // namespace transport
