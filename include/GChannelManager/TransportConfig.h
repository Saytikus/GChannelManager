#pragma once

#include <QHostAddress>
#include <QString>
#include <chrono>

// =====================================================================
//  Конфигурация транспортов.
//  Эти структуры передаются в конструктор конкретного транспорта:
//      auto t = std::make_unique<SerialTransport>(SerialConfig{...});
//  Здесь — только данные, без зависимости от QtSerialPort, чтобы
//  контракт оставался "лёгким".
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
    QHostAddress localAddress  = QHostAddress::AnyIPv4;
    quint16      localPort     = 0;       // 0 — любой свободный
    QHostAddress remoteAddress;           // адрес узла назначения
    quint16      remotePort    = 0;
    bool         bindBeforeSend = true;   // привязать сокет при open()
};

} // namespace transport
