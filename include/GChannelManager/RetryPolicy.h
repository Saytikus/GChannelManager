#pragma once

#include <QtGlobal>
#include <chrono>

// =====================================================================
//  Политика повторов для "отправки с ожиданием ответа".
//  Используется Gateway::sendRequest().
// =====================================================================
struct RetryPolicy {
    qint32 maxRetries = 3;                          // повторов ПОСЛЕ первой попытки
    std::chrono::milliseconds timeout{1000};        // ожидание ответа на попытку
    double backoffFactor = 1.5;                     // множитель таймаута на повтор (double — IEEE 754, фикс. размер)
    std::chrono::milliseconds maxTimeout{15000};    // потолок таймаута попытки
};
