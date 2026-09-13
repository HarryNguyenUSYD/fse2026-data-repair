# Patchouli batch-size benchmark

This directory is self-contained: copy it anywhere to build and run it. It bundles
Patchouli, its JSON dependency and tests, six stdin validators, the harness, and
the exact 600 cases from the original suite. It requires Python 3.9 or newer,
a C++20 compiler, and Make (or CMake 3.20 or newer). Windows with MSYS2 UCRT64,
Linux, and macOS are supported.

## Experiment

`shared-suite/suite-config.json` specifies batch sizes `[1, 2, 4, 8, -1]`.
For each batch size, the runner completes n = 0, 1, 2, 3, 4, 5 in order before
starting the next batch. Each phase runs all 600 cases; cases within a phase
run concurrently. The full benchmark has 30 phases and 18,000 case runs.

Patchouli enumerates unique minimum-edit-cost candidates, scores and sorts them,
and retains the top `ngrams_batch_size` candidates, or all available candidates
when fewer exist. `-1` retains the entire sorted list. Each iteration submits all unseen retained candidates in one oracle process
and selects the first accepted repair in their existing ranked order. For n = 0, scores are tied and existing deterministic
tie-breaking still applies.

All other settings match the original configuration: seed 0, workers -1
(process affinity where available, otherwise logical CPU count), a 300-second
timeout per case, and unbounded Patchouli resource limits. Training examples,
corruptions, scoring validators, and measurement/summary semantics are preserved.

## Run

From this directory, with compiler, Python, and Make on PATH:

```sh
make -j4 all
make check
make smoke
make test
```

`make check` runs Python harness regressions and C++ tests with assertions enabled.
`make smoke` runs the first bundled case across all 30 configurations.
`make test` launches the full 18,000-run benchmark. Neither command regenerates
cases. `make generate` explicitly regenerates them; this is unnecessary for the
bundled experiment and can invalidate the recorded case hash.

Equivalent CMake workflow (use Debug to enable assertions throughout C++ tests):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
python3 -m unittest test_runner_support.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
python3 run_smoke_tests.py
python3 run_tests.py
```

On Windows, use `python` instead of `python3` if appropriate. CMake's executables
must be in this directory's `build/`, as shown above. In an MSYS2 UCRT64 shell,
`make` uses its available Python and g++ toolchain.

## Results and verification

Results stay in this directory's `results/`:

- `benchmark.csv` and `benchmark-summary.json` for the full experiment.
- `benchmark-smoke.csv` and `benchmark-smoke-summary.json` for smoke runs.

CSV rows retain `implementation: patchouli`, numeric `n` and `ngrams_batch_size`,
and existing timing, accuracy, timeout, memory, and RSR diagnostics. Progress and
summary groups use `patchouli-batch-{batch}-n-{n}` (including `batch--1`).
Summaries report six n configurations, five batch configurations, and 30
configurations per case. The retained `implementations_per_case` field also
counts the 30 configurations for compatibility with the original summary shape.

Measurement totals include successful non-error cases only. Failed edit-distance
and wall-time medians preserve the original infinity handling: JSON uses `null`
and an explicit `*_is_infinite` flag. RSR summaries retain enumeration-completeness
and candidate-retention diagnostics.

`oracle_execution_time_ns` records cumulative wall time across all oracle calls
made by Patchouli for a case, including the initial corrupt-string check and
accepted or rejected repair candidates. A steady clock measures each complete
call, including process startup, stdin transfer, execution, waiting for exit,
and cleanup. It excludes the harness's separate final-output validation.
The value is emitted in Patchouli JSON and each benchmark CSV row, and summed
per batch/n configuration under `successful_case_subalgorithm_total_time_ns`.
As with other timings, killed/failed processes without a result have a blank
CSV value and are excluded from summary totals. Existing result files must be
regenerated to contain this measurement; it cannot be recovered retrospectively.

The regression tests verify the bundled case count and SHA-256 against
`shared-suite/test-cases/test-cases.sha256`, phase order, batch/n propagation,
configuration isolation, timeout handling, and CSV/summary counts. C++ tests
cover sorted candidate prefixes and unlimited retention. No parent-directory
files, binaries, configuration, or results are required.

## Batched oracle protocol

All six validators read one JSON array of strings from stdin until EOF, then
write one JSON array of booleans to stdout in the same order. For example,
`validate_ipv4` maps `["192.168.0.1","invalid"]` to `[true,false]`.
Empty arrays return `[]`; duplicate strings retain separate result positions.
Exit code 0 means the request was processed successfully, even if all entries
are false. Malformed requests and execution errors return nonzero exit codes.
The previous raw-string/exit-code verdict interface is replaced; rebuild both
Patchouli and the validators together. Format acceptance rules are unchanged.

The oracle API is `accepts_batch(const std::vector<std::string>&)`, returning
`std::vector<bool>`. The initial input and harness final validation use singleton
lists. Empty oracle lists do not spawn a process. Each repair iteration sends
one complete retained list (including unlimited retention); all entries are
validated, even after an accepted entry. Only when all are rejected are they
added to the negative examples. There is no oracle-call budget setting.
The existing case timeout and separate harness validation timeout still apply.

Oracle wall time includes JSON serialization/parsing and the full subprocess
lifetime. Existing timing CSV columns and summary totals remain unchanged.
Batching can change runtime and timeout outcomes; rerun benchmarks for new
measurements. Recorded results and generated cases are not migrated.

`make check` runs harness, validator protocol, large-payload oracle integration,
and C++ algorithm tests. CMake/CTest runs the C++ and oracle integration tests;
run `python -m unittest test_runner_support.py` for the harness checks.
