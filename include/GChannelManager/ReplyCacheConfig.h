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
//
//  Contract: a corrId must uniquely identify one request payload for the
//  cache's lifetime. The cache has no TTL — eviction is purely LRU-by-count —
//  so within a session a peer must not recycle a corrId for a different
//  request. Across sessions this is safe automatically: the Gateway clears the
//  cache on every session boundary (start/stop, SessionStart/SessionStop
//  received, transport close), so a reconnecting peer that restarts its corrIds
//  cannot collide with stale entries.
// =====================================================================
struct ReplyCacheConfig {
    bool   enabled    = false;
    qint32 maxEntries = 256;   // must be >= 1 when enabled (clamped otherwise)
};
