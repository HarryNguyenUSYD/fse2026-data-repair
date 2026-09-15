#!/usr/bin/env python3
"""Benchmark Patchouli, betaMax, and eRepair on every generated case."""

from runner_support import run_benchmark, worker_count


if __name__ == "__main__":
    run_benchmark(
        worker_count(), implementations=("patchouli", "betamax", "erepair")
    )
