# GChannelManager — documentation

> 🌐 **English** | [Русский](../ru/README.md)

**GChannelManager** is a Qt6/C++20 shared library for building a protocol communication channel on top of an arbitrary transport (serial port, UDP, RUDP, etc.). The central object is `Gateway`: it manages channel state, a keep-alive session, sending/receiving messages, retries and statistics.

> [!NOTE] Where it fits
> Anywhere you need reliable exchange of short frames over an unstable link: telemetry, industrial buses, radio channels, command protocols between a microcontroller and a host.

## Contents

1. [Overview](01-Overview.md) — what it is, why, key features
2. [Architecture](02-Architecture.md) — layers and components, diagram
3. [States and transitions](03-States-and-Transitions.md) — `ChannelState`, `SessionState`
4. [Protocol and codec](04-Protocol-and-Codec.md) — `IMessageCodec`, frame format
5. [Transport](05-Transport.md) — `ITransport` and its implementations
6. [Gateway API](06-Gateway-API.md) — the class's public interface
7. [Statistics](07-Statistics.md) — `GatewayStats`, `statsUpdated`
8. [Build and integration](08-Build-and-Integration.md) — CMake, options
9. [Testing](09-Testing.md) — Qt Test, `FakeTransport`
10. [User guide](10-User-Guide.md) — step-by-step examples

## Quick entity navigation

| Entity | File | Description |
|---|---|---|
| `Gateway` | [Gateway API](06-Gateway-API.md) | Main class — manages channel, session, sending |
| `GatewayRequest` | [GatewayRequest](06-Gateway-API.md#gatewayrequest) | Descriptor of a request awaiting a reply |
| `GatewayStats` | [Statistics](07-Statistics.md) | POD snapshot of counters |
| `ITransport` | [Transport](05-Transport.md) | Transport interface |
| `IMessageCodec` | [Protocol and codec](04-Protocol-and-Codec.md) | Codec interface |
| `SimpleFrameCodec` | [SimpleFrameCodec](04-Protocol-and-Codec.md#simpleframecodec) | Reference codec (example) |
| `RetryPolicy` | [RetryPolicy](06-Gateway-API.md#retrypolicy) | Retry policy |
| `KeepAliveConfig` | [KeepAliveConfig](06-Gateway-API.md#keepaliveconfig) | Heartbeat settings |
