"""Run generated repair cases against Patchouli across batch sizes and n values."""

from __future__ import annotations

import csv
import json
import math
import os
import platform
import copy
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent
SHARED_SUITE = ROOT / "shared-suite"
BUILD = ROOT / "build"
CASES = SHARED_SUITE / "test-cases" / "test-cases.json"
CONFIG = SHARED_SUITE / "suite-config.json"
RESULTS = ROOT / "results"
FORMATS = ("date", "time", "url", "isbn", "ipv4", "ipv6")
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
EXECUTABLES = {
    "patchouli": BUILD / f"patchouli{EXE_SUFFIX}",
}
STDIN_VALIDATORS = {
    name: BUILD / f"validate_{name}{EXE_SUFFIX}" for name in FORMATS
}
RESULT_FIELDS = (
    "implementation",
    "n",
    "max_rsr_candidates",
    "ngrams_batch_size",
    "case_index",
    "case_id",
    "format",
    "category",
    "positive_examples",
    "negative_examples",
    "regex",
    "corrupt_string",
    "valid_source",
    "true_edit_distance",
    "output_string",
    "accuracy",
    "output_in_positive_examples",
    "observed_edit_distance",
    "effective_seed",
    "total_execution_time_ns",
    "rsr_execution_time_ns", "oracle_execution_time_ns",
    "edsm_execution_time_ns",
    "ngrams_execution_time_ns",
    "initial_state_merge_ns",
    "merge_replay_ns",
    "resumed_state_merge_ns",
    "candidate_copy_or_rollback_ns",
    "negative_validation_ns",
    "total_iterations",
    "rsr_total_calls",
    "rsr_candidates_generated",
    "rsr_max_candidates_in_call",
    "rsr_calls_with_multiple_candidates",
    "rsr_enumeration_truncated",
    "rsr_iterations",
    "wall_time_seconds",
    "peak_memory_bytes",
    "timed_out",
    "error",
    "return_code",
    "stdout_tail",
    "stderr_tail",
)


class ProgressReporter:
    """Print algorithm and overall counters plus a once-per-second timer."""

    def __init__(
        self,
        label: str,
        algorithm_total: int,
        total_executions: int,
        total_offset: int,
        started: float,
    ) -> None:
        self.label = label
        self.algorithm_total = algorithm_total
        self.total_executions = total_executions
        self.total_offset = total_offset
        self.algorithm_completed = 0
        self.started = started
        self._line_width = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._timer, daemon=True)

    def _text(self) -> str:
        elapsed = int(time.monotonic() - self.started)
        overall_completed = self.total_offset + self.algorithm_completed
        return (
            f"[{elapsed:4d}s] {self.label}: completed "
            f"{self.algorithm_completed}/{self.algorithm_total} "
            f"(total {overall_completed}/{self.total_executions})"
        )

    def _write(self, permanent: bool) -> None:
        text = self._text()
        padding = " " * max(0, self._line_width - len(text))
        sys.stdout.write("\r" + text + padding + ("\n" if permanent else ""))
        sys.stdout.flush()
        self._line_width = 0 if permanent else len(text)

    def _timer(self) -> None:
        while not self._stop.wait(1.0):
            with self._lock:
                self._write(permanent=False)

    def start(self) -> None:
        with self._lock:
            self._write(permanent=False)
        self._thread.start()

    def advance(self) -> None:
        with self._lock:
            self.algorithm_completed += 1
            self._write(permanent=True)

    def close(self) -> None:
        self._stop.set()
        self._thread.join()
        with self._lock:
            if self._line_width:
                self._write(permanent=True)


def edit_distance(left: str, right: str) -> int:
    previous = list(range(len(right) + 1))
    for row, left_character in enumerate(left, 1):
        current = [row]
        for column, right_character in enumerate(right, 1):
            current.append(min(
                current[-1] + 1,
                previous[column] + 1,
                previous[column - 1] + (left_character != right_character),
            ))
        previous = current
    return previous[-1]


def load_config() -> dict[str, Any]:
    value = json.loads(CONFIG.read_text(encoding="utf-8"))
    timeout = float(value["case_timeout_seconds"])
    if timeout != -1 and timeout <= 0:
        raise ValueError("case_timeout_seconds must be -1 or positive")
    return value


def load_cases() -> list[dict[str, Any]]:
    if not CASES.exists():
        raise FileNotFoundError(f"{CASES} does not exist; run make generate first")
    value = json.loads(CASES.read_text(encoding="utf-8"))
    if not isinstance(value, list):
        raise ValueError(f"{CASES} must contain a JSON array")
    return value


def n_configurations(config: dict[str, Any]) -> list[int]:
    settings = config["patchouli"]
    if "k_values" in settings:
        raise ValueError("patchouli.k_values is no longer supported; remove it")
    n_values = [int(value) for value in settings["n_values"]]
    if any(value < 0 for value in n_values):
        raise ValueError("n values must be non-negative")
    if len(set(n_values)) != len(n_values):
        raise ValueError("n list must not contain duplicates")
    if not n_values:
        raise ValueError("no patchouli n configurations configured")
    return n_values

def batch_configurations(config: dict[str, Any]) -> list[int]:
    settings = config["patchouli"]
    if "ngrams_batch_size" in settings:
        raise ValueError("use patchouli.ngrams_batch_sizes instead of ngrams_batch_size")
    values = settings.get("ngrams_batch_sizes")
    if (not isinstance(values, list) or not values
            or any(type(value) is not int or (value != -1 and value < 1) for value in values)):
        raise ValueError("ngrams_batch_sizes must be a nonempty list of positive integers or -1")
    if len(set(values)) != len(values):
        raise ValueError("ngrams_batch_sizes must not contain duplicates")
    return list(values)


def phase_label(batch: int, n: int) -> str:
    return f"patchouli-batch-{batch}-n-{n}"


def worker_count() -> int:
    configured = int(load_config()["workers"])
    if configured != -1:
        if configured < 1:
            raise ValueError("configured workers must be -1 or at least 1")
        return configured
    try:
        affinity = os.sched_getaffinity(0)
        if affinity:
            return len(affinity)
    except (AttributeError, NotImplementedError, OSError):
        pass
    return os.cpu_count() or 1


def _ensure_binaries() -> None:
    paths = (
        *EXECUTABLES.values(),
        *STDIN_VALIDATORS.values(),
    )
    missing = [path for path in paths if not path.is_file()]
    if missing:
        listing = "\n".join(f"  {path}" for path in missing)
        raise FileNotFoundError("Run make first; missing executables:\n" + listing)


def _kill_process_group(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGKILL)
            return
        except ProcessLookupError:
            return
    process.kill()


def _tail(text: str, lines: int = 10) -> str:
    return "\n".join(text.strip().splitlines()[-lines:])


def _validator_accepts(format_name: str, output: str) -> bool:
    completed = subprocess.run(
        [str(STDIN_VALIDATORS[format_name].resolve())],
        input=json.dumps([output]),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=10,
        check=False,
    )
    if completed.returncode != 0:
        detail = _tail(completed.stderr or completed.stdout, lines=3)
        suffix = f": {detail}" if detail else ""
        raise RuntimeError(
            f"scoring validator exited with code {completed.returncode}{suffix}"
        )
    verdicts = json.loads(completed.stdout)
    if not isinstance(verdicts, list) or len(verdicts) != 1 or type(verdicts[0]) is not bool:
        raise ValueError("scoring validator must return one boolean in a JSON array")
    return verdicts[0]


def _current_config(
    suite_config: dict[str, Any], validator: Path, n: int
) -> dict[str, Any]:
    settings = suite_config["patchouli"]
    return {
        "seed": int(suite_config["seed"]),
        "oracle": {"executable": str(validator.resolve())},
        "repair": {
            "n": n,
            "ngrams_batch_size": int(settings["ngrams_batch_size"]),
            "max_candidate_length": int(settings["max_candidate_length"]),
        },
        "limits": {
            "max_iterations": int(settings["max_iterations"]),
            "max_states": int(settings["max_states"]),
            "max_queue_size": int(settings["max_queue_size"]),
            "max_rsr_candidates": int(settings["max_rsr_candidates"]),
        },
    }


def _launch(
    arguments: list[str], working_directory: Path, timeout: float | None,
    stdout_path: Path | None = None,
) -> tuple[str, str, int, bool]:
    stdout_stream = (
        stdout_path.open("w", encoding="utf-8") if stdout_path is not None else None
    )
    try:
        process = subprocess.Popen(
            arguments,
            cwd=working_directory,
            stdout=stdout_stream if stdout_stream is not None else subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=(os.name == "posix"),
        )
        timed_out = False
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            _kill_process_group(process)
            stdout, stderr = process.communicate()
    finally:
        if stdout_stream is not None:
            stdout_stream.close()
    if stdout_path is not None:
        with stdout_path.open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            size = stream.tell()
            stream.seek(max(0, size - 65536))
            stdout = stream.read().decode("utf-8", errors="replace")
    return stdout, stderr, process.returncode, timed_out


def execute_case(
    implementation: str,
    n: int | None,
    case_index: int,
    case: dict[str, Any],
    suite_config: dict[str, Any],
) -> dict[str, Any]:
    started = time.perf_counter()
    configured_timeout = float(suite_config["case_timeout_seconds"])
    timeout = None if configured_timeout == -1 else configured_timeout
    timed_out = False
    stdout = ""
    stderr = ""
    return_code: int | None = None
    output = ""
    execution_time: int | str = ""
    peak_memory: int | str = ""
    effective_seed: int | str = ""
    oracle_execution_time: int | str = ""
    rsr_execution_time: int | str = ""
    edsm_execution_time: int | str = ""
    ngrams_execution_time: int | str = ""
    initial_state_merge_time: int | str = ""
    merge_replay_time: int | str = ""
    resumed_state_merge_time: int | str = ""
    candidate_copy_or_rollback_time: int | str = ""
    negative_validation_time: int | str = ""
    total_iterations: int | str = ""
    rsr_total_calls: int | str = ""
    rsr_candidates_generated: int | str = ""
    rsr_max_candidates_in_call: int | str = ""
    rsr_calls_with_multiple_candidates: int | str = ""
    rsr_enumeration_truncated: bool | str = ""
    rsr_iterations = ""
    error = ""
    try:
        with tempfile.TemporaryDirectory(prefix=f"{implementation}-") as directory:
            working_directory = Path(directory)
            if implementation == "patchouli":
                if n is None:
                    raise ValueError("patchouli requires n")
                input_json = {
                    "positive_examples": case["positive_examples"],
                    "negative_examples": case["negative_examples"],
                    "corrupt_string": case["corrupt_string"],
                }
                config_json = _current_config(
                    suite_config, STDIN_VALIDATORS[case["format"]], n
                )
                (working_directory / "input.json").write_text(
                    json.dumps(input_json, indent=2) + "\n", encoding="utf-8"
                )
                (working_directory / "config.json").write_text(
                    json.dumps(config_json, indent=2) + "\n", encoding="utf-8"
                )
                arguments = [str(EXECUTABLES["patchouli"].resolve())]
            else:
                raise ValueError(f"unknown implementation: {implementation}")

            stdout, stderr, return_code, timed_out = _launch(
                arguments, working_directory, timeout,
            )

        if timed_out:
            error = f"Algorithm exceeded {configured_timeout:g} seconds."
        elif return_code != 0:
            error = f"Process exited with code {return_code}."
        elif implementation == "patchouli":
            parsed = json.loads(stdout)
            required = {
                "output_string", "effective_seed", "peak_memory_bytes",
                "total_execution_time_ns", "rsr_execution_time_ns", "oracle_execution_time_ns",
                "edsm_execution_time_ns",
                "ngrams_execution_time_ns", "initial_state_merge_ns",
                "merge_replay_ns", "resumed_state_merge_ns",
                "candidate_copy_or_rollback_ns", "negative_validation_ns",
                "total_iterations", "rsr_total_calls",
                "rsr_candidates_generated", "rsr_max_candidates_in_call",
                "rsr_calls_with_multiple_candidates",
                "rsr_enumeration_truncated", "rsr_iterations"
            }
            if not isinstance(parsed, dict) or set(parsed) != required:
                raise ValueError("patchouli stdout has an unexpected JSON shape")
            if not isinstance(parsed["output_string"], str):
                raise ValueError("patchouli output_string is not a string")
            if not isinstance(parsed["peak_memory_bytes"], int):
                raise ValueError("patchouli peak_memory_bytes is not an integer")
            if not isinstance(parsed["total_execution_time_ns"], int):
                raise ValueError("patchouli total_execution_time_ns is not an integer")
            if not isinstance(parsed["effective_seed"], int):
                raise ValueError("patchouli effective_seed is not an integer")
            for field in (
                "rsr_execution_time_ns", "oracle_execution_time_ns",
                "edsm_execution_time_ns", "ngrams_execution_time_ns",
                "initial_state_merge_ns", "merge_replay_ns",
                "resumed_state_merge_ns", "candidate_copy_or_rollback_ns",
                "negative_validation_ns",
                "total_iterations",
            ):
                if not isinstance(parsed[field], int):
                    raise ValueError(f"patchouli {field} is not an integer")
            for field in (
                "rsr_total_calls", "rsr_candidates_generated",
                "rsr_max_candidates_in_call",
                "rsr_calls_with_multiple_candidates",
            ):
                if not isinstance(parsed[field], int):
                    raise ValueError(f"patchouli {field} is not an integer")
            if not isinstance(parsed["rsr_enumeration_truncated"], bool):
                raise ValueError(
                    "patchouli rsr_enumeration_truncated is not boolean"
                )
            if not isinstance(parsed["rsr_iterations"], list):
                raise ValueError("patchouli rsr_iterations is not an array")
            expected_iteration_fields = {
                "minimum_edit_cost", "unique_candidates",
                "candidates_after_ngrams", "enumeration_complete",
            }
            for iteration in parsed["rsr_iterations"]:
                if (
                    not isinstance(iteration, dict)
                    or set(iteration) != expected_iteration_fields
                ):
                    raise ValueError(
                        "patchouli RSR iteration has an unexpected JSON shape"
                    )
                integer_fields = (
                    "minimum_edit_cost", "unique_candidates",
                    "candidates_after_ngrams",
                )
                if (
                    not all(isinstance(iteration[field], int) for field in integer_fields)
                    or not isinstance(iteration["enumeration_complete"], bool)
                ):
                    raise ValueError(
                        "patchouli RSR iteration has invalid field types"
                    )
            if len(parsed["rsr_iterations"]) != parsed["rsr_total_calls"]:
                raise ValueError(
                    "patchouli RSR call count does not match iteration details"
                )
            output = parsed["output_string"]
            effective_seed = parsed["effective_seed"]
            execution_time = parsed["total_execution_time_ns"]
            peak_memory = parsed["peak_memory_bytes"]
            oracle_execution_time = parsed["oracle_execution_time_ns"]
            rsr_execution_time = parsed["rsr_execution_time_ns"]
            edsm_execution_time = parsed["edsm_execution_time_ns"]
            ngrams_execution_time = parsed["ngrams_execution_time_ns"]
            initial_state_merge_time = parsed["initial_state_merge_ns"]
            merge_replay_time = parsed["merge_replay_ns"]
            resumed_state_merge_time = parsed["resumed_state_merge_ns"]
            candidate_copy_or_rollback_time = parsed["candidate_copy_or_rollback_ns"]
            negative_validation_time = parsed["negative_validation_ns"]
            total_iterations = parsed["total_iterations"]
            rsr_total_calls = parsed["rsr_total_calls"]
            rsr_candidates_generated = parsed["rsr_candidates_generated"]
            rsr_max_candidates_in_call = parsed["rsr_max_candidates_in_call"]
            rsr_calls_with_multiple_candidates = parsed[
                "rsr_calls_with_multiple_candidates"
            ]
            rsr_enumeration_truncated = parsed["rsr_enumeration_truncated"]
            rsr_iterations = json.dumps(
                parsed["rsr_iterations"], separators=(",", ":")
            )
    except Exception as exception:
        error = f"{type(exception).__name__}: {exception}"

    elapsed = time.perf_counter() - started
    accepted = False
    if not error:
        try:
            accepted = _validator_accepts(case["format"], output)
            if not accepted:
                error = "Repair was rejected by the format validator."
        except Exception as exception:
            error = f"{type(exception).__name__}: {exception}"


    return {
        "implementation": implementation,
        "n": "" if n is None else n,
        "max_rsr_candidates": suite_config["patchouli"]["max_rsr_candidates"] if implementation == "patchouli" else "",
        "ngrams_batch_size": suite_config["patchouli"]["ngrams_batch_size"] if implementation == "patchouli" else "",
        "case_index": case_index,
        "case_id": case["case_id"],
        "format": case["format"],
        "category": case["category"],
        "positive_examples": json.dumps(case["positive_examples"], separators=(",", ":")),
        "negative_examples": json.dumps(case["negative_examples"], separators=(",", ":")),
        "regex": case["regex"],
        "corrupt_string": case["corrupt_string"],
        "valid_source": case["valid_source"],
        "true_edit_distance": case["true_edit_distance"],
        "output_string": output,
        "accuracy": int(accepted),
        "output_in_positive_examples": int(output in case["positive_examples"]),
        "observed_edit_distance": edit_distance(case["corrupt_string"], output),
        "effective_seed": effective_seed,
        "total_execution_time_ns": execution_time,
        "oracle_execution_time_ns": oracle_execution_time,
        "rsr_execution_time_ns": rsr_execution_time,
        "edsm_execution_time_ns": edsm_execution_time,
        "ngrams_execution_time_ns": ngrams_execution_time,
        "initial_state_merge_ns": initial_state_merge_time,
        "merge_replay_ns": merge_replay_time,
        "resumed_state_merge_ns": resumed_state_merge_time,
        "candidate_copy_or_rollback_ns": candidate_copy_or_rollback_time,
        "negative_validation_ns": negative_validation_time,
        "total_iterations": total_iterations,
        "rsr_total_calls": rsr_total_calls,
        "rsr_candidates_generated": rsr_candidates_generated,
        "rsr_max_candidates_in_call": rsr_max_candidates_in_call,
        "rsr_calls_with_multiple_candidates": rsr_calls_with_multiple_candidates,
        "rsr_enumeration_truncated": rsr_enumeration_truncated,
        "rsr_iterations": rsr_iterations,
        "wall_time_seconds": elapsed,
        "peak_memory_bytes": peak_memory,
        "timed_out": int(timed_out),
        "error": error,
        "return_code": "" if return_code is None else return_code,
        "stdout_tail": _tail(stdout),
        "stderr_tail": _tail(stderr),
    }


def _run_phase(
    implementation: str,
    n: int | None,
    cases: list[dict[str, Any]],
    config: dict[str, Any],
    workers: int,
    total_offset: int,
    total_executions: int,
    timer_started: float,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any] | None] = [None] * len(cases)
    label = phase_label(config["patchouli"]["ngrams_batch_size"], n)
    print(f"{label}: {len(cases)} cases, {workers} workers", flush=True)
    progress = ProgressReporter(
        label, len(cases), total_executions, total_offset, timer_started
    )
    progress.start()
    try:
        with ProcessPoolExecutor(max_workers=workers) as pool:
            futures = {
                pool.submit(execute_case, implementation, n, index, case, config): index
                for index, case in enumerate(cases)
            }
            for future in as_completed(futures):
                index = futures[future]
                rows[index] = future.result()
                progress.advance()
    finally:
        progress.close()
    return [row for row in rows if row is not None]


def _write_csv(
    path: Path, fieldnames: tuple[str, ...], rows: list[dict[str, Any]]
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    temporary.replace(path)


def _summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    observed = [row for row in rows if not row["timed_out"] and not row["error"]]
    wall_times = [
        float(row["wall_time_seconds"])
        if not row["timed_out"] and not row["error"]
        else float("inf")
        for row in rows
    ]
    edit_distances = [
        int(row["observed_edit_distance"])
        if row["accuracy"] and not row["error"]
        else float("inf")
        for row in rows
    ]
    measurement_fields = (
        "rsr_execution_time_ns", "oracle_execution_time_ns",
        "edsm_execution_time_ns", "ngrams_execution_time_ns",
        "initial_state_merge_ns", "merge_replay_ns",
        "resumed_state_merge_ns", "candidate_copy_or_rollback_ns",
        "negative_validation_ns",
    )
    measurement_samples = {
        field: [int(row[field]) for row in observed if row[field] != ""]
        for field in measurement_fields
    }
    measurement_totals = {
        field: (sum(values) if values else None)
        for field, values in measurement_samples.items()
    }
    iteration_samples = [
        int(row["total_iterations"])
        for row in observed
        if row["total_iterations"] != ""
    ]
    median_edit_distance = (
        statistics.median(edit_distances) if edit_distances else None
    )
    median_edit_distance_is_infinite = (
        isinstance(median_edit_distance, float)
        and not math.isfinite(median_edit_distance)
    )
    median_wall_time = statistics.median(wall_times) if wall_times else None
    median_wall_time_is_infinite = (
        isinstance(median_wall_time, float)
        and not math.isfinite(median_wall_time)
    )
    summary = {
        "cases": len(rows),
        "observed_runtime_samples": len(observed),
        "censored_timeout_samples": sum(int(row["timed_out"]) for row in rows),
        "accepted_repairs": sum(int(row["accuracy"]) for row in rows),
        "timeouts": sum(int(row["timed_out"]) for row in rows),
        "errors": sum(bool(row["error"]) for row in rows),
        "measurement_scope": "successful non-error cases only",
        "successful_case_total_iterations": (
            sum(iteration_samples) if iteration_samples else None
        ),
        "successful_case_subalgorithm_total_time_ns": measurement_totals,
        "outputs_in_positive_examples": sum(
            int(row["output_in_positive_examples"]) for row in rows
        ),
        "median_observed_edit_distance": (
            None if median_edit_distance_is_infinite else median_edit_distance
        ),
        "median_observed_edit_distance_is_infinite": (
            median_edit_distance_is_infinite
        ),
        "median_wall_time_seconds": (
            None if median_wall_time_is_infinite else median_wall_time
        ),
        "median_wall_time_seconds_is_infinite": median_wall_time_is_infinite,
    }
    if rows and rows[0]["implementation"] == "patchouli":
        summary["rsr_candidate_diagnostics"] = {
            "total_calls": sum(
                int(row["rsr_total_calls"]) for row in observed
            ),
            "unique_candidates_generated": sum(
                int(row["rsr_candidates_generated"]) for row in observed
            ),
            "max_candidates_in_one_call": max(
                (int(row["rsr_max_candidates_in_call"]) for row in observed),
                default=0,
            ),
            "calls_with_multiple_candidates": sum(
                int(row["rsr_calls_with_multiple_candidates"])
                for row in observed
            ),
            "successful_cases_with_truncated_enumeration": sum(
                int(row["rsr_enumeration_truncated"])
                for row in observed
            ),
            "all_successful_enumerations_complete": all(
                row["rsr_enumeration_truncated"] is False for row in observed
            ) if observed else None,
        }
    return summary


def _environment_metadata() -> dict[str, Any]:
    compiler = os.environ.get("CXX", "c++")
    flags = os.environ.get(
        "CXXFLAGS", "-O2 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic"
    )
    try:
        completed = subprocess.run(
            [compiler, "--version"], capture_output=True, text=True, timeout=10,
            check=False,
        )
        compiler_version = _tail(completed.stdout or completed.stderr, lines=1)
    except (OSError, subprocess.SubprocessError) as exception:
        compiler_version = f"unavailable: {type(exception).__name__}: {exception}"
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python_version": platform.python_version(),
        "compiler_command": compiler,
        "compiler_version": compiler_version,
        "compiler_flags": flags,
    }


def run_benchmark(
    workers: int,
    case_limit: int | None = None,
    result_stem: str = "benchmark",
) -> None:
    if workers < 1:
        raise ValueError("workers must be at least 1")
    config = load_config()
    n_values = n_configurations(config)
    batch_values = batch_configurations(config)
    _ensure_binaries()
    cases = load_cases()
    if case_limit is not None:
        if case_limit < 1:
            raise ValueError("case_limit must be at least 1")
        cases = cases[:case_limit]
    started = time.perf_counter()
    timer_started = time.monotonic()

    total_executions = len(cases) * (len(n_values) * len(batch_values))
    all_rows: list[dict[str, Any]] = []
    summaries: dict[str, Any] = {}
    total_offset = 0
    for batch in batch_values:
        for n in n_values:
            phase_config = copy.deepcopy(config)
            phase_config["patchouli"]["ngrams_batch_size"] = batch
            label = phase_label(batch, n)
            rows = _run_phase(
                "patchouli", n, cases, phase_config, workers,
                total_offset=total_offset, total_executions=total_executions,
                timer_started=timer_started,
            )
            all_rows.extend(rows)
            summaries[label] = {
                "n": n,
                "max_rsr_candidates": int(config["patchouli"]["max_rsr_candidates"]),
                "ngrams_batch_size": batch,
                **_summary(rows),
            }
            total_offset += len(cases)

    results_path = RESULTS / f"{result_stem}.csv"
    summary_path = RESULTS / f"{result_stem}-summary.json"
    _write_csv(results_path, RESULT_FIELDS, all_rows)
    summary = {
        "workers": workers,
        "seed": config["seed"],
        "case_timeout_seconds": config["case_timeout_seconds"],
        "n_configurations": len(n_values),
        "base_cases": len(cases),
        "implementations_per_case": len(n_values) * len(batch_values),
        "configurations_per_case": len(n_values) * len(batch_values),
        "batch_configurations": len(batch_values),
        "total_case_runs": total_executions,
        "environment": _environment_metadata(),
        "suite_config": config,
        "elapsed_wall_time_seconds": time.perf_counter() - started,
        "implementations": summaries,
    }
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(
        json.dumps(summary, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {results_path}", flush=True)
    print(f"wrote {summary_path}", flush=True)
