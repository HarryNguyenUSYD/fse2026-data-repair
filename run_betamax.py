#!/usr/bin/env python3
"""Run only betaMax on the shared benchmark cases."""

import argparse

from runner_support import run_benchmark, worker_count


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", action="store_true", help="run only the first case")
    args = parser.parse_args()
    run_benchmark(
        worker_count(), case_limit=1 if args.smoke else None,
        result_stem="betamax-smoke" if args.smoke else "betamax",
        implementations=("betamax",),
    )
