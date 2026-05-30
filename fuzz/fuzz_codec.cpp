// libFuzzer target for SimpleFrameCodec::feed() — the untrusted-byte entry point.
//
// feed()/parse() consume attacker-controlled bytes and are pure (no event loop,
// timers, or signals), which makes them an ideal fuzz target and a permanent
// regression guard for the bounded-length / CRC / resync hardening. Build it
// under -fsanitize=fuzzer,address,undefined (see fuzz/CMakeLists.txt) so any
// out-of-bounds read, UB, or unbounded growth surfaces immediately.
//
// The input is split into fuzzer-chosen chunks to also exercise feed()'s
// cross-call buffering of partial frames.

#include "GChannelManager/SimpleFrameCodec.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    SimpleFrameCodec codec;
    size_t i = 0;
    while (i < size) {
        const size_t n = data[i] % (size - i) + 1;   // 1..remaining
        codec.feed(QByteArray(reinterpret_cast<const char *>(data + i),
                              static_cast<qsizetype>(n)));
        i += n;
    }
    return 0;
}
