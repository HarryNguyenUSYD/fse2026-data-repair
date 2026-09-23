# Patchouli batch-size benchmark

This directory is self-contained: copy it anywhere to build and run it. It bundles
Patchouli, betaMax, eRepair, validators, the harness, and the exact 600 cases
from the original suite. It requires Python 3.9 or newer, a C++20 compiler, and
Make (or CMake 3.20 or newer). All three algorithms support Windows, Linux, and
macOS through the shared oracle launcher.

## Experiment

`shared-suite/suite-config.json` specifies n = 2 and batch sizes
`[1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64, -1]`.
The runner finishes each batch/n phase before starting the next. Each phase
runs all 600 cases concurrently. Including betaMax and eRepair, the full
benchmark has 14 configurations and 8,400 case runs.

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
# Run only one comparison algorithm:
make test-patchouli
make test-betamax
make test-erepair
```

`make check` runs the C++ Patchouli tests with assertions enabled.
`make smoke` runs the first bundled case across every configured Patchouli
batch/n combination plus betaMax and eRepair. `make test` runs the same three
algorithms over the full case set. Neither command regenerates cases. `make
generate` explicitly regenerates them; this is unnecessary for the bundled
experiment and replaces the bundled cases.

`make test-patchouli`, `make test-betamax`, and `make test-erepair` build and
run only the named algorithm (plus its validators). The one-case checks are
`make smoke-patchouli`, `make smoke-betamax`, and `make smoke-erepair`.
Aggregate runs retain the existing `results/benchmark*` filenames. Independent
runs write `results/patchouli*`, `results/betamax*`, or `results/erepair*`.
This version replaces eRepair's POSIX temporary-file and shell invocation APIs
with the shared oracle launcher.

Equivalent CMake workflow (use Debug to enable assertions throughout C++ tests):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
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
Summaries report one n configuration, twelve batch configurations, and fourteen
configurations per case for the full comparison. The retained
`implementations_per_case` field also counts configurations.

Measurement totals include successful non-error cases only. Failed edit-distance
and wall-time medians preserve the original infinity handling: JSON uses `null`
and an explicit `*_is_infinite` flag. RSR summaries retain enumeration-completeness
and candidate-retention diagnostics.

`oracle_total_calls`, `oracle_candidates_submitted`, and `oracle_execution_time_ns`
record process calls, submitted strings, and cumulative oracle wall time per case.
They are emitted for Patchouli, betaMax, and eRepair. Patchouli includes the
initial corrupt-string check and accepted or rejected candidate batches. A
steady clock measures preparation, serialization, process launch, execution,
waiting, verdict decoding, and cleanup for all three algorithms. The measurements
exclude the harness's separate final-output validation. CSV rows contain all three fields;
summaries expose `successful_case_total_oracle_calls` and sum oracle time under
`successful_case_subalgorithm_total_time_ns.oracle_execution_time_ns`.
Submitted-string totals use `successful_case_total_oracle_candidates_submitted`;
eRepair's count includes prefix queries and repeats. Each Patchouli batch counts
as one process call and as many submitted strings as it contains.
Oracle metrics survive case timeouts: each algorithm atomically replaces
`oracle-metrics.json` at invocation start and completion. The harness recovers
the last snapshot before killing the process, including the count of the
in-flight invocation and its elapsed time up to timeout detection. Completed
durations use the steady clock; an interrupted duration is estimated from a
Unix wall-clock timestamp (a system clock adjustment can affect that estimate).
Cases killed before their first invocation report zero calls and zero oracle
time. Timeout rows retain their error/timeout status and these recovered CSV
values; summaries aggregate them under `timeout_case_oracle_metrics`, separately
from existing successful-case totals. Other unavailable algorithm timings stay
blank. Existing result files must be
regenerated to contain this measurement; it cannot be recovered retrospectively.

The C++ tests cover Patchouli's algorithm behavior, including sorted candidate
prefixes and unlimited retention. No parent-directory files, binaries,
configuration, or results are required.

## Shared process invocation

`sources/common/oracle_process.hpp` launches every black-box validator directly
using `posix_spawnp` on macOS/Linux and `CreateProcessW` on Windows. No shell is
involved. Standard streams are redirected through private temporary files, so
large input/output cannot block on pipe capacity. This staging cost is included
in oracle timing and changes runtime relative to older results.

Validators and query behavior remain separate: Patchouli uses JSON arrays on
stdin/stdout; `validate_betamax_<format>` reads a candidate filename and returns
0 for accepted or 1 for rejected; `boundary_validate_<format>` additionally
returns 255 for incomplete prefixes. Unexpected exit codes, spawn failures,
and oracle timeouts are errors. betaMax's configured per-oracle timeout still
applies; the harness's case timeout covers all algorithms. Batching, ranking,
prefix search, and existing caching are preserved. Failed invocations are not
included in successful-case summaries. Timeout metrics are reported separately.

## Batched oracle protocol

All six Patchouli validators read one JSON array of strings from stdin until EOF, then
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
lifetime. The new submitted-string field requires rebuilding and rerunning;
old result files are not migrated.
Batching can change runtime and timeout outcomes; rerun benchmarks for new
measurements. Recorded results and generated cases are not migrated.

`make check` and CMake/CTest run the C++ Patchouli algorithm tests.
