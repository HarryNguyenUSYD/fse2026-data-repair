# Patchouli Benchmark Suite

This repository benchmarks Patchouli against betaMax and eRepair on 600 bundled
data-repair cases covering dates, times, URLs, ISBNs, IPv4 addresses, and IPv6
addresses. Patchouli runs with `n = 2` across 12 candidate batch sizes, producing
14 configurations and 8,400 case runs in the full comparison.

The suite is self-contained and supports Windows, Linux, and macOS. It requires
Python 3.9 or newer, a C++20 compiler, and Make. CMake 3.20 or newer is also
supported.

## Run with Make

From the repository root:

```sh
make -j4 check   # Build and run the Patchouli C++ checks
make -j4 smoke   # Run one case through all 14 configurations
make -j4 test    # Run the complete 8,400-case benchmark
```

Run only one implementation with:

```sh
make smoke-patchouli
make smoke-betamax
make smoke-erepair

make test-patchouli
make test-betamax
make test-erepair
```

Use `make clean` to remove generated binaries and results. The bundled cases are
ready to use; `make generate` replaces them from `shared-suite/suite-config.json`.

## Run with CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
python3 run_smoke_tests.py
python3 run_tests.py
```

On Windows, use `python` if `python3` is unavailable. CMake executables must
remain in the repository's `build/` directory for the Python runner to find them.

## Results

Output is written to `results/`:

- `benchmark-smoke.csv` and `benchmark-smoke-summary.json` for `make smoke`.
- `benchmark.csv` and `benchmark-summary.json` for `make test`.
- Implementation-specific files for the individual smoke and test targets.

The CSV and JSON outputs include repair accuracy, edit distance, runtime,
timeouts, memory use, Patchouli RSR diagnostics, and oracle call measurements.
