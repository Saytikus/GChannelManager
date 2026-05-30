# Overview

> 🌐 **English** | [Русский](../ru/01-Обзор.md)

## What it is

`GChannelManager` solves a common task: you have an arbitrary byte transport (serial port, UDP socket, a custom RUDP stack) and your own frame protocol. You need to reliably send commands, receive replies, survive brief link drops, and observe link metrics. The library provides:

- **a channel layer** — opening/closing the transport;
- **a keep-alive session layer** — transition to `Suspended` when the heartbeat is lost, and return to `Active` without reopening the channel (RUDP-style behavior);
- **request/reply with correlation** — `sendRequest(payload) → GatewayRequest`, with automatic retries and exponential backoff;
- **fire-and-forget sending** — `send(payload)`, with no reply expected;
- **statistics** — counters for bytes/requests/heartbeats with a periodic signal.

The library **does not know** your protocol or your transport. You plug those two dependencies in through the [`ITransport`](05-Transport.md) and [`IMessageCodec`](04-Protocol-and-Codec.md) interfaces. For illustration, there is `SimpleFrameCodec` — a simple binary frame protocol.

## Key features

| Feature | Where | More |
|---|---|---|
| Enable/disable the channel | `Gateway::enableChannel()` / `disableChannel()` | [States and transitions](03-States-and-Transitions.md) |
| Session with keep-alive | `Gateway::startSession()` / `stopSession()` | [Session](03-States-and-Transitions.md#session) |
| Keep-alive on the fly | `setKeepAliveEnabled(bool)` / `setKeepAliveConfig(...)` | [Keep-alive](06-Gateway-API.md#keep-alive) |
| Resilience to drops | automatic `Active ↔ Suspended` transition | [States and transitions](03-States-and-Transitions.md) |
| Request/reply | `sendRequest(payload)` → `GatewayRequest*` | [sendRequest](06-Gateway-API.md#sendrequest) |
| Retries with backoff | `RetryPolicy {maxRetries, timeout, backoffFactor, maxTimeout}` | [RetryPolicy](06-Gateway-API.md#retrypolicy) |
| Fire-and-forget | `send(payload) → bool` | [send](06-Gateway-API.md#send) |
| Server role | signal `requestReceived` + slot `reply(corrId, response)` | [Server role](06-Gateway-API.md#server-role-incoming-requests-and-reply) |
| Reply cache (idempotency) | `setReplyCacheConfig(...)`, repeated request served from cache | [Reply cache](06-Gateway-API.md#reply-cache-server-role) |
| Statistics | `stats()`, `setStatsInterval(...)`, signal `statsUpdated` | [Statistics](07-Statistics.md) |

## Minimal example

```cpp
#include <GChannelManager/Gateway.h>
#include <GChannelManager/SimpleFrameCodec.h>
// your transport implementing ITransport:
#include "MySerialTransport.h"

Gateway gw;
gw.setCodec(std::make_unique<SimpleFrameCodec>());
gw.setTransport(std::make_unique<MySerialTransport>(SerialConfig{...}));

QObject::connect(&gw, &Gateway::sessionStateChanged,
    [&](Gateway::SessionState s) {
        if (s == Gateway::SessionState::Active) {
            auto *req = gw.sendRequest(QByteArray("HELLO"));
            QObject::connect(req, &GatewayRequest::succeeded,
                [](const QByteArray &resp) { qInfo() << "got" << resp; });
        }
    });

gw.enableChannel();   // everything from here is event-driven
```

A detailed walkthrough is in the [User guide](10-User-Guide.md).

## Technologies

- **C++20**, no templates/`concepts` are used — plain modern C++ with `[[nodiscard]]`, `enum class`, `std::unique_ptr`, lambdas and `std::chrono`.
- **Qt 6 (Core)** — the only required Qt dependency (also works with Qt 5 if present). The core library does **not** pull in QtNetwork: [`UdpConfig`](05-Transport.md#udpconfig) stores addresses as plain strings.
- **CMake ≥ 3.16**, built with `Ninja`/`Make`/`MSBuild`.
- **Qt Test** — an optional module (only for running the unit tests).
