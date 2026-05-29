# Testing

> 🌐 **English** | [Русский](../ru/09-Тестирование.md)

## Stack

The tests are written with **Qt Test** (`Qt6::Test`). Advantages for a Qt project:

- `QSignalSpy` captures Qt signals without boilerplate;
- `QTRY_VERIFY` / `QTRY_COMPARE_WITH_TIMEOUT` wait for asynchronous conditions inside the event loop;
- `QTEST_MAIN`/`QTEST_APPLESS_MAIN` provide a correct `QCoreApplication` (or work without one);
- out-of-the-box `ctest` integration via `add_test`.

## Running

The tests are built optionally (see [Build and integration](08-Build-and-Integration.md)):

```sh
cmake -S . -B build/Desktop-Debug -DGCHANNELMANAGER_BUILD_TESTS=ON
cmake --build build/Desktop-Debug
cd build/Desktop-Debug
LD_LIBRARY_PATH="$PWD" ctest --output-on-failure
```

Expected output:

```
Test project .../build/Desktop-Debug
    Start 1: SimpleFrameCodec
1/2 Test #1: SimpleFrameCodec .................   Passed    0.01 sec
    Start 2: Gateway
2/2 Test #2: Gateway ..........................   Passed    0.33 sec

100% tests passed, 0 tests failed out of 2
```

Running a single test for verbose output:

```sh
./tests/tst_Gateway -v2
./tests/tst_Gateway -v2 -fn "session_*"   # filter by name
```

## Layout

```
tests/
├── CMakeLists.txt          ← find_package(Qt6 COMPONENTS Test) + add_test()
├── FakeTransport.h         ← in-memory ITransport for tests
├── tst_SimpleFrameCodec.cpp ← codec cases
└── tst_Gateway.cpp          ← Gateway cases
```

## `FakeTransport`

The main testing tool. It does no I/O but honestly honors the `ITransport` contract:

| Capability | Method |
|---|---|
| Remembers everything sent | `[[nodiscard]] const QList<QByteArray> &sent() const` |
| Delivers arbitrary bytes "from the peer" | `void simulateReceive(const QByteArray &bytes)` |
| Simulates a transport error | `void simulateError(const QString &msg)` |
| Clears the sent buffer | `void clearSent()` |
| Extra signal for checks | `signal void dataSent(const QByteArray &)` |

> [!TIP]
> A concrete "wait until a Request frame appears" pattern in tests: `QTRY_VERIFY(lastRequestCorrId(t) != 0);` — `lastRequestCorrId` is a local helper in `tst_Gateway.cpp` that iterates over `t->sent()` and parses the frames via `SimpleFrameCodec::parse`.

## What is covered

### `tst_SimpleFrameCodec` (13)

- `encodeRequest_roundtrip` — a `Reply` frame with the same `corrId` parses as `Type::Reply`.
- `incomingRequestFrame_classifiedAsTypeRequest` — a `Request` frame is classified as `Type::Request`, preserving the `corrId`.
- `encodeReply_carriesCorrelation` — a `Reply` frame carries the same `corrId` as the request.
- `encodeData_carriesNoCorrelation` — a `Data` frame, `corrId == 0`, type `Type::Data`.
- `encodeKeepAlive_classifiedAsKeepAliveReplyOnly` — `KeepAliveReq` → `Unknown`, `KeepAliveReply` → `Type::KeepAlive`.
- `encodeSessionStart_classifiedAsType_SessionStart` — a `SessionStart` frame → `Type::SessionStart`.
- `encodeSessionStartAck_classifiedAsType_SessionStartAck` — a `SessionStartAck` frame → `Type::SessionStartAck`.
- `encodeSessionStop_classifiedAsType_SessionStop` — a `SessionStop` frame → `Type::SessionStop`.
- `feed_splitAcrossCalls_buffered` — a frame split in the middle across two `feed()` calls is correctly reassembled.
- `feed_garbagePrefix_resyncsByMagic` — garbage before `0xA5` is skipped byte by byte.
- `feed_multipleFrames_inOneBuffer` — three frames in one `feed()` call → three `DecodedMessage`s.
- `parse_consumesBytesIncrementally` — `parse()` removes what it consumes from the buffer, the "tail" remains.
- `reset_clearsBuffer` — `reset()` zeros the internal buffer.

### `tst_Gateway` (25)

| Group | Cases |
|---|---|
| Channel and session states | `channelEnable_emitsStateAndOpensTransport`, `session_reachesActive_afterKeepAliveReply`, `session_keepAliveDisabled_becomesActiveImmediately`, `startSession_sendsSessionStartFrame`, `stopSession_sendsSessionStopFrame`, `incomingSessionStart_acksAndEntersActive`, `incomingSessionStop_failsPendingAndGoesIdle`, `startSession_timeoutFiresWhenAckMissing` |
| Keep-alive at runtime | `setKeepAliveEnabled_runtimeOff_clearsSuspendedToActive`, `setKeepAliveEnabled_runtimeOn_startsHeartbeat` |
| Request awaiting a reply | `sendRequest_succeedsOnPeerReply`, `sendRequest_retriesOnTimeout_thenSucceeds`, `sendRequest_failsBeforeChannelEnabled`, `cancel_failsPendingRequest` |
| Fire-and-forget | `send_fireAndForget_emitsDataFrame`, `send_failsWhenSessionInactive` |
| Server role and reply cache | `incomingRequest_emitsRequestReceived`, `reply_sendsReplyFrameViaTransport`, `replyCache_disabled_emitsSignalOnEveryRequest`, `replyCache_enabled_resendsCachedReplyWithoutEmittingSignal`, `replyCache_disableClearsExistingEntries` |
| Statistics | `stats_countersTrackKeepAliveAndRequest`, `stats_droppedReplyCounted`, `stats_periodicSignalFires_andStopsOnZeroInterval`, `stats_resetClearsCounters` |

## Patterns for your own tests

### Ready-made setup

```cpp
FakeTransport *wireUp(Gateway &gw,
                      std::chrono::milliseconds ka = std::chrono::milliseconds(40),
                      bool keepAliveEnabled = true);
```

In tests it is convenient to start from this helper — it creates the codec, the fake transport, and applies the keep-alive config.

### Waiting for Active

```cpp
auto *t = wireUp(gw);
gw.enableChannel();
gw.startSession();
// SessionStart has already gone out; once SessionStartAck arrives — the session is Active
t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::SessionStartAck, 0, {}));
QCOMPARE(gw.sessionState(), Gateway::SessionState::Active);
```

### Request round-trip

```cpp
auto *req = gw.sendRequest(QByteArray("ping"));
QSignalSpy ok(req, &GatewayRequest::succeeded);

QTRY_VERIFY(lastRequestCorrId(t) != 0);
const quint32 cid = lastRequestCorrId(t);
t->simulateReceive(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, cid,
                                               QByteArray("ACK")));
QTRY_COMPARE(ok.count(), 1);
```

### Waiting for Suspended via timeout

There is no direct hook — you have to let the keep-alive timer fire `maxMissed + 1` ticks without a reply. If the keep-alive interval in the test is 40 ms and `maxMissed = 3`, then `Suspended` is reached at ~160 ms:

```cpp
QTRY_COMPARE_WITH_TIMEOUT(gw.sessionState(),
                          Gateway::SessionState::Suspended, 1000);
```

## Assertion style

Inside `private slots:` methods the following are used:

| Macro | When |
|---|---|
| `QCOMPARE(actual, expected)` | comparing values |
| `QVERIFY(cond)` | an arbitrary condition |
| `QTRY_VERIFY(cond)` | same, but spins the event loop for up to 5 s (changeable via `QTRY_VERIFY_WITH_TIMEOUT`) |
| `QTRY_COMPARE(actual, expected)` | same, for comparisons |
| `QSignalSpy` | counting/analyzing signals |

`QTRY_*` is the main weapon in tests with timers. Do not write `QThread::msleep` in tests — it blocks the event loop and breaks signal handling.
