#pragma once

#include <QtGlobal>
#include <chrono>
#include <limits>

namespace gcm::internal {

// QTimer::start()/setInterval() take int milliseconds. Clamp a chrono duration
// to that range so an absurdly large interval (> ~24.8 days, i.e. INT_MAX ms)
// does not silently truncate/overflow into a tiny or negative timer value.
inline int timerMs(std::chrono::milliseconds d)
{
    const auto c = d.count();
    if (c <= 0)
        return 0;
    if (c > std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    return static_cast<int>(c);
}

} // namespace gcm::internal
