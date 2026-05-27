#pragma once

#include <QtGlobal>
#include <chrono>

// =====================================================================
//  Конфигурация keep-alive (поддержка редкой/нестабильной связи).
//  Применяется через Gateway::setKeepAliveConfig() — изменения подхватываются
//  на лету в работающей сессии (старт/стоп heartbeat, смена интервала).
// =====================================================================
struct KeepAliveConfig {
    bool   enabled = true;
    std::chrono::milliseconds interval{2000};
    qint32 maxMissed = 3;   // пропусков подряд до перехода в Suspended
};
