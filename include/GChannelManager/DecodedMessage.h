#pragma once

#include <QByteArray>
#include <QtGlobal>

// =====================================================================
//  Результат разбора одного сообщения кодеком.
//  type определяет, как поле payload (и correlationId для Reply) должно
//  интерпретироваться вызывающим кодом.
// =====================================================================
struct DecodedMessage {
    enum class Type {
        Reply,      // ответ на наш запрос -> смотрим correlationId
        Request,    // запрос от узла к нам -> приложение отвечает Gateway::reply()
        KeepAlive,  // keep-alive (подтверждение живости линка)
        Data,       // данные без корреляции (push от узла)
        Unknown     // не распознано / служебное
    };

    Type       type          = Type::Unknown;
    quint32    correlationId = 0;   // valid для Reply и Request
    QByteArray payload;
};
