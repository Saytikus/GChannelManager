# Fuzzing

`fuzz_codec` is a [libFuzzer](https://llvm.org/docs/LibFuzzer.html) target for
`SimpleFrameCodec::feed()` — the codec's untrusted-byte entry point. It is a
permanent regression guard for the bounded-length / CRC / resync hardening:
under AddressSanitizer + UndefinedBehaviorSanitizer, any out-of-bounds read,
UB, or unbounded buffer growth on *any* input fails the run immediately.

## Requirements

- **Clang** (libFuzzer ships with LLVM). The target is gated behind an option
  and refuses to configure under other compilers.

## Build & run

```sh
cmake -S . -B build/fuzz -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DGCHANNELMANAGER_BUILD_FUZZERS=ON
cmake --build build/fuzz

# fuzz for 60 seconds against a corpus directory (created if missing)
./build/fuzz/fuzz/fuzz_codec -max_total_time=60 corpus/
```

## Seeding the corpus

Seed `corpus/` with known-good frames so the fuzzer starts from valid inputs
and mutates outward (a frame is `[magic][type][corrId][len][payload][crc16]`).
Any bytes produced by `SimpleFrameCodec::makeFrame(...)` — e.g. the frames built
in `tests/tst_SimpleFrameCodec.cpp` — make good seeds: dump them to files under
`corpus/`. The fuzzer also synthesizes inputs from scratch, so an empty corpus
still works; seeding only speeds up coverage.

## On a crash

libFuzzer writes the offending input to `crash-<hash>`. Reproduce with:

```sh
./build/fuzz/fuzz/fuzz_codec crash-<hash>
```

Then commit those exact bytes as a permanent case in
`tests/tst_SimpleFrameCodec.cpp` (feed them and assert no crash / correct
behavior), so the regression is locked in without needing the fuzzer.
