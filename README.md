# fse2026-new-test

Self-contained benchmark comparing patchouli All-Min, the legacy `betamax-old`
implementation and epsilonrepair on the same deterministic
repair cases. The six formats are date, time, URL, ISBN, IPv4, and IPv6. Epsilonrepair uses
tri-state boundary validators: exit status 0 means a complete valid input, 1
means an invalid prefix, and 255 means an incomplete valid prefix.

Generation is controlled by `shared-suite/suite-config.json`. `N` is the base
case count per corruption level and is divided evenly among the six formats.
The generator produces `N` cases at every edit distance from `d_min` through
`d_max` (1-5 by default). Each entry under `string_lengths` independently
controls the valid strings generated for that data type.

## Configurations

patchouli uses EDSM to rank all RPNI red/blue-fringe merge candidates directly,
without k-tails filtering. It runs the configured n sweep:

```text
n: 0, 1, 2, 3, 4, 5
ngrams_batch_size: 1
max_rsr_candidates: -1
```

For every repair iteration, patchouli runs RSR once and exhaustively enumerates
all unique candidates at the minimum edit cost. It then ranks those candidates
with the n-gram model and queries the single highest-ranked candidate. The
`max_rsr_candidates: -1` setting leaves enumeration uncapped. Timeouts and the
other resource limits can still interrupt an execution; the CSV and summary
diagnostics distinguish complete enumeration from truncation.

All patchouli resource limits are unbounded. `betamax-old` runs with unbounded
repair attempts, edit cost, and per-oracle timeout, and uses a candidate batch
size of 1 (`--attempt-candidates 1` and `--max-candidates 1`); mutation and
equivalence-query sampling are disabled. patchouli and `betamax-old` use seed
0. Every implementation has a 600-second outer timeout; epsilonrepair is
deterministic and does not expose a random seed.

There are six patchouli n configurations, one `betamax-old` configuration,
and one `epsilonrepair` configuration:

```text
120 cases per level x 5 levels = 600 test cases
600 test cases x 8 configurations = 4800 test-case runs
```

Each configuration runs as a separate phase. Within each phase, `workers: -1`
uses every CPU allowed by process affinity, falling back to operating-system
logical CPU count.

## Run

Requirements are a C++20 compiler, Make, Python 3.9 or newer, and a POSIX
environment. Epsilonrepair uses `/tmp`, `mkstemp`, POSIX process-status APIs,
and a shell command for each oracle invocation, so it must be run on Linux,
macOS, or WSL rather than as a native Windows executable.

```sh
make test
```

For a one-base-case validation run across all configurations (8 runs):

```sh
make smoke
```

The equivalent CMake workflow is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
python3 run_smoke_tests.py
```

Results are written under `results/`:

- `benchmark[-smoke].csv`: long-form results for every implementation/configuration.
- `benchmark[-smoke]-summary.json`: aggregate results for each patchouli n value,
  `betamax-old` and `epsilonrepair`, plus base-case and total-run counts. Measurement totals
  cover successful, non-error cases only. Failed repair edit distances count as
  infinite; strict JSON represents an infinite median as `null` with
  `median_observed_edit_distance_is_infinite: true`. Wall-time medians count
  timeouts and errors as infinite in the same way; their JSON flag is
  `median_wall_time_seconds_is_infinite`. Means are not reported.
- patchouli rows also contain per-RSR-call candidate counts, minimum edit costs,
  n-gram retention counts, and enumeration-completeness flags. Their summaries
  aggregate calls that returned multiple minimum-cost candidates and report
  whether all successful enumerations were complete.

The patchouli executable is `build/patchouli`, its source is under
`sources/patchouli`, and its suite configuration key is `patchouli`.
Result labels are `patchouli-n-0` through `patchouli-n-5`; CSVs omit `k`
and k-tails timing fields. Summaries report `n_configurations`.
Obsolete `state_merging.k` and `patchouli.k_values` settings are rejected.

Historical 300-second results and build artifacts are preserved outside this
suite under `../benchmark-archive/k-test-suite/`. New runs write fresh results
under this suite's `results/` directory.

No implementation receives the expected regex, hidden valid source, or true
edit distance. The legacy adapter writes positive, negative, and broken string
files and uses filename-based validators. Epsilonrepair receives only the
broken string and its tri-state boundary oracle. Hidden scoring
fields are used only by the harness after execution. Final accuracy is checked
by the same compiled validator implementation used elsewhere in the benchmark,
not by the descriptive regex stored in each case.

## Oracle rules

The C++ sources in `validators/` match the repository's original validators.
`validator.cpp` provides stdin and file validation; `boundary_validator.cpp`
provides epsilonrepair's tri-state prefix interface.

Membership checks syntax only: date and time shapes, ISBN digit/separator
structure, four IPv4 digit groups, eight IPv6 hexadecimal groups, and the
original URL regular expression. Calendar validity, time ranges, ISBN checksums,
and IPv4 numeric ranges are not checked. Status 255 means the input can be
extended to match the format.

The generator uses the same syntax rules to label examples and corruptions.
Regenerate cases with `python3 shared-suite/generate_cases.py`.

Run harness regression tests with `python3 -m unittest test_runner_support.py`.
