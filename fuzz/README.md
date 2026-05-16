# Fuzz harnesses

Coverage-guided fuzzing for the YAML / CSV / JSON parsers via libFuzzer.

## Why fuzz

The hand-crafted corpus in `tests/test_parser_robustness.c` catches the
malformed shapes a human thought of. Fuzzing finds the ones nobody
thought of — and during initial wiring it already found a leak in
`csv.read` on a 2-byte unterminated-quote input (`0x22 0xd7`).

## Build

Fuzz binaries are off by default. Opt in via:

```bash
rm -rf build && cmake -S . -B build -DBETL_FUZZ=ON -G Ninja
ninja -C build build_yaml_fuzz build_csv_fuzz build_json_fuzz
```

Requires `clang` + `libclang-rt-<N>-dev` (compiler-rt runtime). The main
gcc build is untouched — `BETL_FUZZ` only adds new custom targets that
explicitly drive clang. Binaries land at `build/fuzz/<name>_fuzz`.

## Run

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

- The fuzz harness itself: `-fsanitize=fuzzer,address,undefined`
  (coverage-guided mutation + ASan + UBSan oracle).
- `libbetl_core.a`: NOT instrumented (gcc-built, no coverage). ASan
  still catches heap bugs across the boundary because the
  AddressSanitizer hooks `malloc` / `free` globally.

The practical effect: bugs in parser code that touch the heap (buffer
overruns, use-after-free, leaks) are caught the moment they happen.
Pure-logic bugs that don't manifest as memory errors won't surface
unless they crash or hang.

## Tips

- Always run with a `-max_total_time` budget. Long runs (~30 min) find
  more, but iterate quickly with 60s during development.
- Seeds in `fuzz/seeds/<target>/` are small valid inputs that libFuzzer
  mutates as starting points. Add interesting valid samples here.
- `-rss_limit_mb=512` caps memory per process so a runaway allocation
  doesn't OOM the host.
- `-timeout=10` makes any single input that takes >10s a finding —
  catches infinite loops as well as crashes.
