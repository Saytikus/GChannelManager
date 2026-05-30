#pragma once

#include <QtGlobal>
#include <chrono>

// =====================================================================
//  Keep-alive configuration (support for a rare/unstable link).
//  Applied via Gateway::setKeepAliveConfig() — changes are picked up
//  on the fly in a running session (start/stop heartbeat, interval change).
// =====================================================================
struct KeepAliveConfig {
    bool   enabled = true;
    std::chrono::milliseconds interval{2000};
    qint32 maxMissed = 3;   // misses in a row before going Suspended; must be >= 0
                            // (a negative value is clamped to 0)
};
