#pragma once

#include <QtGlobal>

// =====================================================================
//  Кэш ответов (idempotency cache).
//
//  Если включён — Gateway запоминает каждый успешно отправленный
//  Gateway::reply(corrId, response). Если узел повторно пришлёт Request
//  с тем же corrId (потому что наш ответ потерялся по дороге), Gateway
//  сам перешлёт сохранённый ответ, НЕ эмитя requestReceived заново.
//
//  Эвикция — LRU на основе QCache, до maxEntries записей.
//  По умолчанию выключен.
// =====================================================================
struct ReplyCacheConfig {
    bool   enabled    = false;
    qint32 maxEntries = 256;
};
