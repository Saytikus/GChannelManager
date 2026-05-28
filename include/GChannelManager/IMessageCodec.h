#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <vector>

#include "DecodedMessage.h"

// =====================================================================
//  Контракт кодека протокольного уровня.
//
//  Гейтвей сам не знает формат кадров вашего протокола. Кодек:
//    * упаковывает запрос в кадр, вшивая correlationId (для сопоставления
//      ответа с запросом при "отправке с ожиданием ответа");
//    * формирует keep-alive кадр;
//    * разбирает входящий поток байт на сообщения и классифицирует их.
//
//  feed() должен буферизовать неполные кадры между вызовами.
// =====================================================================
class IMessageCodec {
public:
    virtual ~IMessageCodec() = default;

    // Упаковать запрос в кадр.
    [[nodiscard]] virtual QByteArray encodeRequest(quint32 correlationId,
                                                   const QByteArray &payload) = 0;

    // Упаковать ответ на входящий запрос — корреляция совпадает с запросом.
    // Используется Gateway::reply().
    [[nodiscard]] virtual QByteArray encodeReply(quint32 correlationId,
                                                 const QByteArray &payload) = 0;

    // Упаковать "несвязанные" данные (fire-and-forget): корреляция не нужна,
    // ответа не ожидается. Используется Gateway::send().
    [[nodiscard]] virtual QByteArray encodeData(const QByteArray &payload) = 0;

    // Кадры жизненного цикла сессии — отдельный канал от keep-alive.
    [[nodiscard]] virtual QByteArray encodeSessionStart()    = 0;
    [[nodiscard]] virtual QByteArray encodeSessionStartAck() = 0;
    [[nodiscard]] virtual QByteArray encodeSessionStop()     = 0;

    // Сформировать keep-alive кадр.
    [[nodiscard]] virtual QByteArray encodeKeepAlive() = 0;

    // Скормить сырые байты, получить список разобранных сообщений.
    [[nodiscard]] virtual std::vector<DecodedMessage> feed(const QByteArray &bytes) = 0;

    // Сбросить внутренний буфер (например, при разрыве/рестарте сессии).
    virtual void reset() {}
};
