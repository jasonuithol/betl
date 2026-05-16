# Fuzz harnesses

Coverage-guided fuzzing for the YAML / CSV / JSON parsers via libFuzzer.

## Why fuzz

The hand-crafted corpus in `tests/test_parser_robustness.c` catches the
malformed shapes a human thought of. Fuzzing finds the ones nobody
thought of — and during initial wiring it already found a leak in
`csv.read` on a 2-byte unterminated-quote input (`0x22 0xd7`).

## Build

Fuzz binaries live in a parallel build tree (`build-fuzz/`) where the
WHOLE codebase is rebuilt with clang + libFuzzer + ASan + UBSan. The
main `build/` (gcc) is untouched.

```bash
cmake -S . -B build-fuzz -DBETL_FUZZ=ON -G Ninja
ninja -C build-fuzz yaml_fuzz csv_fuzz json_fuzz
```

Requires `clang` + `libclang-rt-<N>-dev` (compiler-rt: libFuzzer / ASan
runtime libraries). On bookworm / LLVM 14 that's `libclang-rt-14-dev`.

## Run

The helper script handles configure + build on first invocation, then
just runs subsequent times:

```bash
fuzz/run-fuzz.sh yaml 60       # 60 seconds
fuzz/run-fuzz.sh csv  300      # 5 minutes
fuzz/run-fuzz.sh json 60
```

Crashes / leaks / hangs are written to `fuzz/findings/<target>/` as
binary reproducers. The path is gitignored — commit a finding to
`tests/test_parser_robustness.c` (as a regression case) once you fix
the underlying bug.

## Reproducing a finding

Each finding file is the exact input that triggered the bug. To
reproduce:

```bash
build/fuzz/csv_fuzz fuzz/findings/csv/leak-abc123...
```

Run under `gdb` to step through the crash, or pipe through `xxd` to
inspect the bytes.

## What's instrumented

The entire build-fuzz/ tree is compiled with:

- `-fsanitize=fuzzer-no-link` — coverage probes throughout `libbetl_core`
  so libFuzzer's mutator gets real coverage signal from the parser
  internals (not just from the harness file).
- `-fsanitize=address` — heap bug oracle: out-of-bounds, use-after-free,
  double-free, leaks.
- `-fsanitize=undefined` — UB oracle: signed overflow, null-deref,
  alignment violations, OOB shifts.

Fuzz binaries additionally link with `-fsanitize=fuzzer` (no `-no-link`
suffix) to pull in libFuzzer's `main()` driver.

The practical effect: every branch decision inside csv.read,
yaml_load, json.read is visible to the mutator, so libFuzzer can
quickly converge on the input shapes that hit uncovered code paths.

## Tips

- Always run with a `-max_total_time` budget. Long runs (~30 min) find
  more, but iterate quickly with 60s during development.
- Seeds in `fuzz/seeds/<target>/` are small valid inputs that libFuzzer
  mutates as starting points. Add interesting valid samples here.
- `-rss_limit_mb=512` caps memory per process so a runaway allocation
  doesn't OOM the host.
- `-timeout=10` makes any single input that takes >10s a finding —
  catches infinite loops as well as crashes.
