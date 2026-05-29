#pragma once

#include <QtGlobal>

// =====================================================================
//  Reply cache (idempotency cache).
//
//  When enabled, the Gateway remembers every successfully sent
//  Gateway::reply(corrId, response). If the peer resends a Request with
//  the same corrId (because our reply was lost on the way), the Gateway
//  resends the stored reply itself, WITHOUT re-emitting requestReceived.
//
//  Eviction is LRU, backed by QCache, up to maxEntries entries.
//  Disabled by default.
// =====================================================================
struct ReplyCacheConfig {
    bool   enabled    = false;
    qint32 maxEntries = 256;
};
