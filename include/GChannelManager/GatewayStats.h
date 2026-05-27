#pragma once

#include <QMetaType>
#include <QtGlobal>

// =====================================================================
//  Снимок счётчиков активности Gateway. Дёшёвая POD-структура —
//  можно периодически эмитить через statsUpdated() в дашборды/логи.
// =====================================================================
struct GatewayStats {
    quint64 sentBytes          = 0;   // всего ушло в transport->send()
    quint64 recvBytes          = 0;   // всего пришло из bytesReceived
    quint64 requestsSent       = 0;   // sendRequest, прошедшие предусловия
    quint64 requestsSucceeded  = 0;
    quint64 requestsFailed     = 0;
    quint64 retries            = 0;   // повторных попыток суммарно
    quint64 fireAndForgetSent  = 0;   // вызовов send(...) с успехом
    quint64 keepAlivesSent     = 0;
    quint64 keepAlivesReceived = 0;
    quint64 suspensions        = 0;   // переходов Active/Establishing -> Suspended
    quint64 droppedReplies     = 0;   // Reply без сопоставленного pending
    quint64 dataReceived       = 0;   // несвязанных Data-сообщений
    quint64 incomingRequests   = 0;   // Request-кадров от узла
    quint64 cachedRepliesResent = 0;  // ответов, повторно отправленных из кэша
};

Q_DECLARE_METATYPE(GatewayStats)
