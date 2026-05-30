#pragma once

#include <QtGlobal>
#include <chrono>

// =====================================================================
//  Retry policy for "send and await a reply".
//  Used by Gateway::sendRequest().
// =====================================================================
struct RetryPolicy {
    qint32 maxRetries = 3;                          // retries AFTER the first attempt
    std::chrono::milliseconds timeout{1000};        // wait for a reply per attempt
    double backoffFactor = 1.5;                     // timeout multiplier per retry; >= 1.0
                                                    // (values < 1.0 are floored to 1.0 = no backoff)
    std::chrono::milliseconds maxTimeout{15000};    // per-attempt timeout ceiling
};
