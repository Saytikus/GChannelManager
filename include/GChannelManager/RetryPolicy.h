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
    double backoffFactor = 1.5;                     // timeout multiplier per retry (double — IEEE 754, fixed size)
    std::chrono::milliseconds maxTimeout{15000};    // per-attempt timeout ceiling
};
