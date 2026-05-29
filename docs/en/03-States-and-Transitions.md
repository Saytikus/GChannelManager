# States and transitions

> 🌐 **English** | [Русский](../ru/03-Состояния-и-переходы.md)

`Gateway` hosts two independent state machines: the **channel** (the physical transport) and the **session** (the logical protocol with heartbeat). The channel can be enabled without an active session. A session cannot exist without an enabled channel.

## Channel

The channel reflects whether the transport is on. There are two states:

```mermaid
stateDiagram-v2
    [*] --> Disabled
    Disabled --> Enabled: enableChannel()<br/>transport.open()<br/>onTransportOpened
    Enabled --> Disabled: disableChannel()<br/>or transport.closed
```

| State | Meaning | What you can do |
|---|---|---|
| `Disabled` | Transport closed | `enableChannel()`; cannot `startSession()` |
| `Enabled`  | Transport open and ready to pass bytes | `startSession()`; `disableChannel()` |

> [!NOTE]
> `enableChannel()` is asynchronous: the actual transition to `Enabled` happens after `ITransport::opened()`. Subscribe to the `channelStateChanged` signal to react to readiness.

## Session

The session is a logical "conversation" on top of the channel: keep-alive confirms the link is alive, requests are correlated by `correlationId`. The session has five states:

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> Establishing: startSession()<br/>send SessionStart

    Establishing --> Active: SessionStartAck received<br/>(if keep-alive on — heartbeat starts)
    Establishing --> Idle: SessionStartAck not received<br/>within sessionStartTimeout

    Idle --> Active: peer sent SessionStart<br/>(we sent SessionStartAck)

    Active --> Suspended: maxMissed heartbeats missed in a row
    Suspended --> Active: KeepAliveReply received

    Active --> Idle: peer sent SessionStop<br/>(pending failed)
    Suspended --> Idle: peer sent SessionStop

    Active --> Stopping: stopSession()<br/>send SessionStop
    Establishing --> Stopping: stopSession()
    Suspended --> Stopping: stopSession()
    Stopping --> Idle: all pending failed<br/>timers stopped
```

| State | Description |
|---|---|
| `Idle` | No session. `sendRequest`/`send` → `SessionInactive` |
| `Establishing` | Session is starting: `SessionStart` sent, waiting for `SessionStartAck` from the peer. Timeout — `sessionStartTimeout` |
| `Active` | Session confirmed; requests flow as usual. Keep-alive (if enabled) starts exactly here |
| `Suspended` | The link has been "silent" `maxMissed+1` times in a row (keep-alive does not reply). The channel is still open. Requests are still queued as pending and may time out |
| `Stopping` | Transient state inside `stopSession()` — a `SessionStop` is sent |

> [!TIP] Why `Suspended`
> On an unstable radio channel, short "dropouts" are normal. Closing the transport and reopening it is expensive. `Suspended` lets you ride out a drop without losing the established settings (port, socket parameters), and automatically returns to `Active` as soon as the heartbeat replies.

## Tying the channel and session together

```mermaid
sequenceDiagram
    participant User as User
    participant GW as Gateway
    participant T as ITransport
    participant C as IMessageCodec

    User->>GW: enableChannel()
    GW->>T: open()
    T-->>GW: opened
    Note over GW: channelStateChanged(Enabled)

    User->>GW: startSession()
    Note over GW: sessionStateChanged(Establishing)
    GW->>C: encodeSessionStart()
    C-->>GW: frame
    GW->>T: send(SessionStart)
    T-->>GW: bytesReceived(ack)
    GW->>C: feed(ack)
    C-->>GW: DecodedMessage Type=SessionStartAck
    Note over GW: sessionStateChanged(Active)<br/>+ keep-alive timer starts
```

## What happens on a drop

If the transport is silent for `keepAlive.maxMissed + 1` ticks in a row:

1. The Gateway itself, without touching the transport, moves the session to `Suspended`.
2. The channel stays `Enabled` — bytes may keep flowing, the peer simply isn't replying.
3. When at least one `KeepAliveReply` arrives, `onKeepAliveReply()` resets the counter and returns to `Active`.

```mermaid
stateDiagram-v2
    direction LR
    Active --> Suspended: missed > maxMissed
    Suspended --> Active: reply received
    note right of Suspended
        The channel is NOT closed.
        Pending requests keep
        existing and time out
        per their RetryPolicy.
    end note
```

## Enabling and disabling keep-alive on the fly

`setKeepAliveEnabled(bool)` and `setKeepAliveConfig(...)` work at any time:

| Was / became | Session `Active` | Session `Establishing` | Session `Suspended` |
|---|---|---|---|
| `false → true` | Starts the heartbeat timer, sends the first kalive | Same | Same |
| `true → false` | Timer stops; missed counter is reset | Goes to `Active` | Goes to `Active` |
| only `interval` changed | `QTimer::setInterval(...)` | same | same |

Rationale: if the heartbeat is off, there can be no missed beats, so "the link is alive by default".

See the implementation: `src/Gateway.cpp:133` (`setKeepAliveConfig`).

## Transition signals

```cpp
signals:
    void channelStateChanged(Gateway::ChannelState state);
    void sessionStateChanged(Gateway::SessionState state);
    void keepAliveEnabledChanged(bool enabled);
```

Subscribe **before** calling `enableChannel()`/`startSession()`, otherwise you may miss the early transitions.
