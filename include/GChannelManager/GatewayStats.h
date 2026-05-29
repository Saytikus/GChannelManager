#pragma once

#include <QMetaType>
#include <QtGlobal>

// =====================================================================
//  A snapshot of Gateway activity counters. A cheap POD struct —
//  can be emitted periodically via statsUpdated() to dashboards/logs.
// =====================================================================
struct GatewayStats {
    quint64 sentBytes          = 0;   // total bytes handed to transport->send()
    quint64 recvBytes          = 0;   // total bytes arrived via bytesReceived
    quint64 requestsSent       = 0;   // sendRequest calls that passed preconditions
    quint64 requestsSucceeded  = 0;
    quint64 requestsFailed     = 0;
    quint64 retries            = 0;   // retry attempts in total
    quint64 fireAndForgetSent  = 0;   // successful send(...) calls
    quint64 keepAlivesSent     = 0;
    quint64 keepAlivesReceived = 0;
    quint64 suspensions        = 0;   // Active/Establishing -> Suspended transitions
    quint64 droppedReplies     = 0;   // Reply with no matching pending request
    quint64 dataReceived       = 0;   // uncorrelated Data messages
    quint64 incomingRequests   = 0;   // Request frames from the peer
    quint64 cachedRepliesResent = 0;  // replies resent from the cache
    quint64 sessionStartsSent       = 0;   // SessionStart frames sent
    quint64 sessionStartsReceived   = 0;   // SessionStart frames received from the peer
    quint64 sessionStartTimeouts    = 0;   // SessionStartAck not received in time
    quint64 sessionStopsSent        = 0;
    quint64 sessionStopsReceived    = 0;
};

Q_DECLARE_METATYPE(GatewayStats)
