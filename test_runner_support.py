"""Contract tests for the n-only benchmark harness (no executables required)."""

import contextlib
import copy
import csv
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import runner_support as runner


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.config = runner.load_config()
        self.case = runner.load_cases()[0]

    def test_n_values(self):
        self.assertEqual(runner.n_configurations(self.config), list(range(6)))
        for values in ([], [-1], [0, 0]):
            with self.subTest(values=values):
                config = copy.deepcopy(self.config)
                config["patchouli"]["n_values"] = values
                with self.assertRaises(ValueError):
                    runner.n_configurations(config)

    def test_obsolete_settings_fail_before_build(self):
        self.config["patchouli"]["k_values"] = [0]
        with patch.object(runner, "load_config", return_value=self.config), \
                patch.object(runner, "_ensure_binaries") as ensure:
            with self.assertRaisesRegex(ValueError, "k_values.*remove it"):
                runner.run_benchmark(1)
            ensure.assert_not_called()

    def test_generated_config(self):
        config = runner._current_config(self.config, Path("oracle"), 0)
        self.assertEqual(config["repair"]["n"], 0)
        self.assertNotIn("state_merging", config)
        self.assertNotIn("k", config)

    def test_600_second_timeout_applies_to_every_algorithm(self):
        self.assertEqual(self.config["case_timeout_seconds"], 600)
        for implementation in runner.EXECUTABLES:
            with self.subTest(implementation=implementation), \
                    patch.object(runner, "_launch", return_value=("", "", -9, True)) as launch:
                row = runner.execute_case(
                    implementation, 0 if implementation == "patchouli" else None,
                    0, self.case, self.config,
                )
                self.assertEqual(launch.call_args.args[2], 600)
                self.assertEqual(row["timed_out"], 1)
                self.assertEqual(row["accuracy"], 0)

    def test_all_algorithms_and_oracle_interfaces_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executables = {name: root / name for name in runner.EXECUTABLES}
            stdin = {name: root / f"validate_{name}" for name in runner.FORMATS}
            files = {name: root / f"validate_old_{name}" for name in runner.FORMATS}
            boundary = {name: root / f"boundary_validate_{name}" for name in runner.FORMATS}
            paths = [*executables.values(), *stdin.values(), *files.values(), *boundary.values()]
            for path in paths:
                path.touch()
            with patch.object(runner, "EXECUTABLES", executables), \
                    patch.object(runner, "STDIN_VALIDATORS", stdin), \
                    patch.object(runner, "FILE_VALIDATORS", files), \
                    patch.object(runner, "BOUNDARY_VALIDATORS", boundary):
                runner._ensure_binaries()
                for path in paths:
                    path.unlink()
                    with self.assertRaises(FileNotFoundError):
                        runner._ensure_binaries()
                    path.touch()

    def test_launch_timeout_keeps_diagnostics(self):
        with patch.object(runner.subprocess, "Popen") as popen, \
                patch.object(runner, "_kill_process_group") as kill:
            process = popen.return_value
            process.returncode = -9
            process.communicate.side_effect = [
                runner.subprocess.TimeoutExpired("patchouli", 1), ("partial output", "diagnostic")]
            result = runner._launch(["patchouli"], Path("."), 1)
            self.assertEqual(result, ("partial output", "diagnostic", -9, True))
            kill.assert_called_once_with(process)

    def patchouli_result(self):
        return {
            "output_string": self.case["valid_source"],
            "effective_seed": 0,
            "peak_memory_bytes": 100,
            "total_execution_time_ns": 100,
            "rsr_execution_time_ns": 10,
            "edsm_execution_time_ns": 10,
            "ngrams_execution_time_ns": 10,
            "initial_state_merge_ns": 10,
            "merge_replay_ns": 0,
            "resumed_state_merge_ns": 0,
            "candidate_copy_or_rollback_ns": 1,
            "negative_validation_ns": 1,
            "total_iterations": 1,
            "rsr_total_calls": 1,
            "rsr_candidates_generated": 1,
            "rsr_max_candidates_in_call": 1,
            "rsr_calls_with_multiple_candidates": 0,
            "rsr_enumeration_truncated": False,
            "rsr_iterations": [{
                "minimum_edit_cost": 1, "unique_candidates": 1,
                "candidates_after_ngrams": 1, "enumeration_complete": True,
            }],
        }

    def execute_mock_result(self, result):
        with patch.object(runner, "_launch", return_value=(json.dumps(result), "", 0, False)), \
                patch.object(runner, "_validator_accepts", return_value=True):
            return runner.execute_case("patchouli", 0, 0, self.case, self.config)

    def test_result_schema_and_summary(self):
        row = self.execute_mock_result(self.patchouli_result())
        self.assertEqual(row["error"], "")
        self.assertEqual(row["n"], 0)
        self.assertEqual(set(row), set(runner.RESULT_FIELDS))
        self.assertNotIn("k", row)
        self.assertNotIn("ktails_execution_time_ns", row)
        times = runner._summary([row])["successful_case_subalgorithm_total_time_ns"]
        self.assertEqual(times["rsr_execution_time_ns"], 10)
        self.assertNotIn("ktails_execution_time_ns", times)

    def test_result_validation_preserved(self):
        result = self.patchouli_result()
        result["ktails_execution_time_ns"] = 0
        self.assertIn("unexpected JSON shape", self.execute_mock_result(result)["error"])
        result = self.patchouli_result()
        result["rsr_execution_time_ns"] = "bad"
        self.assertIn("not an integer", self.execute_mock_result(result)["error"])

    def test_n_zero_phase_label_and_dispatch(self):
        # Use completed futures to exercise submission and collection without processes.
        from concurrent.futures import Future

        def submit(function, *args):
            self.assertIs(function, runner.execute_case)
            self.assertEqual(args[:3], ("patchouli", 0, 0))
            future = Future()
            future.set_result({"n": 0})
            return future

        output = io.StringIO()
        with patch.object(runner, "ProcessPoolExecutor") as pool, \
                patch.object(runner, "ProgressReporter"), contextlib.redirect_stdout(output):
            pool.return_value.__enter__.return_value.submit.side_effect = submit
            rows = runner._run_phase("patchouli", 0, [self.case], self.config, 1, 0, 8, 0)
        self.assertEqual(rows, [{"n": 0}])
        self.assertIn("patchouli-n-0:", output.getvalue())

    def test_sweep_outputs_and_counts(self):
        expected = [("patchouli", n) for n in range(6)] + [("betamax-old", None), ("epsilonrepair", None)]
        row = self.execute_mock_result(self.patchouli_result())
        for count in (1, 600):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                def phase(implementation, n, cases, config, workers, **kwargs):
                    self.assertEqual(kwargs["total_executions"], count * 8)
                    return [dict(row, implementation=implementation, n=n)
                            for _ in cases]

                with patch.object(runner, "RESULTS", Path(directory)), \
                        patch.object(runner, "load_cases", return_value=[self.case] * count), \
                        patch.object(runner, "_ensure_binaries"), \
                        patch.object(runner, "_environment_metadata", return_value={}), \
                        patch.object(runner, "_run_phase", side_effect=phase) as phases, \
                        contextlib.redirect_stdout(io.StringIO()):
                    runner.run_benchmark(1)
                self.assertEqual([call.args[:2] for call in phases.call_args_list], expected)
                summary = json.loads((Path(directory) / "benchmark-summary.json").read_text())
                self.assertEqual(summary["n_configurations"], 6)
                self.assertEqual(summary["case_timeout_seconds"], 600)
                self.assertEqual(summary["implementations_per_case"], 8)
                self.assertEqual(summary["total_case_runs"], count * 8)
                self.assertNotIn("k_n_combinations", summary)
                self.assertEqual(list(summary["implementations"]), [f"patchouli-n-{n}" for n in range(6)] + ["betamax-old", "epsilonrepair"])
                with (Path(directory) / "benchmark.csv").open(newline="") as stream:
                    reader = csv.DictReader(stream)
                    self.assertEqual(reader.fieldnames, list(runner.RESULT_FIELDS))
                    self.assertEqual(sum(1 for _ in reader), count * 8)


if __name__ == "__main__":
    unittest.main()
