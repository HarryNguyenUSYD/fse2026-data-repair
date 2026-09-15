"""Contract tests for the batch-size benchmark harness (no executables required)."""

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
        self.config["patchouli"]["ngrams_batch_size"] = 1

    def test_n_values(self):
        self.config["patchouli"]["n_values"] = list(range(6))
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

    def test_300_second_timeout_applies_to_every_algorithm(self):
        self.assertEqual(self.config["case_timeout_seconds"], 300)
        for implementation in runner.EXECUTABLES:
            with self.subTest(implementation=implementation), \
                    patch.object(runner, "_launch", return_value=("", "", -9, True)) as launch:
                row = runner.execute_case(
                    implementation, 0 if implementation == "patchouli" else None,
                    0, self.case, self.config,
                )
                self.assertEqual(launch.call_args.args[2], 300)
                self.assertEqual(row["timed_out"], 1)
                self.assertEqual(row["accuracy"], 0)

    def test_patchouli_run_only_requires_patchouli_and_stdin_validators(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executables = {
                name: root / name for name in ("patchouli", "betamax", "erepair")
            }
            stdin = {name: root / f"validate_{name}" for name in runner.FORMATS}
            paths = [executables["patchouli"], *stdin.values()]
            for path in paths:
                path.touch()
            with patch.object(runner, "EXECUTABLES", executables), patch.object(runner, "STDIN_VALIDATORS", stdin):
                runner._ensure_binaries(("patchouli",))
                for path in paths:
                    path.unlink()
                    with self.assertRaises(FileNotFoundError):
                        runner._ensure_binaries(("patchouli",))
                    path.touch()

    def test_batch_values(self):
        config = runner.load_config()
        self.assertEqual(
            runner.batch_configurations(config),
            config["patchouli"]["ngrams_batch_sizes"],
        )
        for values in ([], [0], [-2], [1, 1], [True], [1.5], ["2"], None, 1):
            with self.subTest(values=values):
                config["patchouli"]["ngrams_batch_sizes"] = values
                with patch.object(runner, "load_config", return_value=config), patch.object(runner, "_ensure_binaries") as ensure:
                    with self.assertRaises(ValueError):
                        runner.run_benchmark(1)
                    ensure.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instead"):
            runner.batch_configurations(self.config)

    def test_batch_parameter_propagation(self):
        for batch in (1, 2, 4, 8, -1):
            for n in range(6):
                self.config["patchouli"]["ngrams_batch_size"] = batch
                def launch(arguments, directory, timeout, stdout_path=None):
                    config = json.loads((directory / "config.json").read_text())
                    self.assertEqual(config["repair"]["ngrams_batch_size"], batch)
                    self.assertEqual(config["repair"]["n"], n)
                    self.assertEqual(config["seed"], 0)
                    self.assertTrue(all(value == -1 for value in config["limits"].values()))
                    self.assertEqual(timeout, 300)
                    return json.dumps(self.patchouli_result()), "", 0, False
                with patch.object(runner, "_launch", side_effect=launch), patch.object(runner, "_validator_accepts", return_value=True):
                    row = runner.execute_case("patchouli", n, 0, self.case, self.config)
                self.assertEqual(row["error"], "")
                self.assertEqual((row["ngrams_batch_size"], row["n"]), (batch, n))

    def test_bundled_cases(self):
        import hashlib
        digest = hashlib.sha256(runner.CASES.read_bytes()).hexdigest()
        expected = (runner.SHARED_SUITE / "test-cases" / "test-cases.sha256").read_text().split()[0]
        self.assertEqual(digest, expected)
        self.assertEqual(len(runner.load_cases()), 600)

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
            "oracle_total_calls": 2,
            "oracle_execution_time_ns": 37,
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
        self.assertEqual(row["oracle_total_calls"], 2)
        self.assertEqual(runner._summary([row])["successful_case_total_oracle_calls"], 2)
        self.assertEqual(row["oracle_execution_time_ns"], 37)
        self.assertEqual(times["oracle_execution_time_ns"], 37)
        self.assertNotIn("ktails_execution_time_ns", times)

    def test_oracle_timing_summary_and_csv(self):
        row = self.execute_mock_result(self.patchouli_result())
        failed = dict(row, timed_out=1, error="timeout", accuracy=0,
                      oracle_execution_time_ns="")
        totals = runner._summary([row, row, failed])["successful_case_subalgorithm_total_time_ns"]
        self.assertEqual(totals["oracle_execution_time_ns"], 74)
        self.assertIsNone(runner._summary([failed])[
            "successful_case_subalgorithm_total_time_ns"]["oracle_execution_time_ns"])
        buffer = io.StringIO()
        writer = csv.DictWriter(buffer, fieldnames=runner.RESULT_FIELDS)
        writer.writeheader()
        writer.writerows([row, failed])
        buffer.seek(0)
        saved = list(csv.DictReader(buffer))
        self.assertEqual(saved[0]["oracle_execution_time_ns"], "37")
        self.assertEqual(saved[1]["oracle_execution_time_ns"], "")

    def test_oracle_timing_required_and_validated(self):
        result = self.patchouli_result()
        del result["oracle_execution_time_ns"]
        self.assertIn("unexpected JSON shape", self.execute_mock_result(result)["error"])
        result["oracle_execution_time_ns"] = "bad"
        self.assertIn("not an integer", self.execute_mock_result(result)["error"])

    def test_oracle_call_count_required_and_validated(self):
        result = self.patchouli_result()
        del result["oracle_total_calls"]
        self.assertIn("unexpected JSON shape", self.execute_mock_result(result)["error"])
        result["oracle_total_calls"] = "bad"
        self.assertIn("not an integer", self.execute_mock_result(result)["error"])

    def test_betamax_oracle_metrics(self):
        stderr = "ORACLE_METRICS total_calls=7 execution_time_ns=1234\n"
        with patch.object(
            runner, "_launch",
            return_value=(self.case["valid_source"] + "\n", stderr, 0, False),
        ), patch.object(runner, "_validator_accepts", return_value=True):
            row = runner.execute_case("betamax", None, 0, self.case, self.config)
        self.assertEqual(row["error"], "")
        self.assertEqual(row["oracle_total_calls"], 7)
        self.assertEqual(row["oracle_execution_time_ns"], 1234)

    def test_erepair_oracle_metrics(self):
        def launch(arguments, directory, timeout, stdout_path=None):
            (directory / "repaired.txt").write_text(
                self.case["valid_source"], encoding="ascii"
            )
            return (
                "ORACLE_METRICS total_calls=9 execution_time_ns=5678\n",
                "", 0, False,
            )

        with patch.object(runner, "_launch", side_effect=launch), \
                patch.object(runner, "_validator_accepts", return_value=True):
            row = runner.execute_case("erepair", None, 0, self.case, self.config)
        self.assertEqual(row["error"], "")
        self.assertEqual(row["oracle_total_calls"], 9)
        self.assertEqual(row["oracle_execution_time_ns"], 5678)

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
        self.assertIn("patchouli-batch-1-n-0:", output.getvalue())

    def test_sweep_outputs_and_counts(self):
        expected = [(batch, n) for batch in (1, 2, 4, 8, -1) for n in range(6)]
        row = self.execute_mock_result(self.patchouli_result())
        original = runner.load_config()
        sweep_config = copy.deepcopy(original)
        sweep_config["patchouli"]["n_values"] = list(range(6))
        sweep_config["patchouli"]["ngrams_batch_sizes"] = [1, 2, 4, 8, -1]
        for count in (1, 600):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as directory:
                observed = []
                configs = []
                def phase(implementation, n, cases, config, workers, **kwargs):
                    batch = config["patchouli"]["ngrams_batch_size"]
                    self.assertEqual(implementation, "patchouli")
                    self.assertEqual(kwargs["total_executions"], count * 30)
                    self.assertEqual(kwargs["total_offset"], len(observed) * count)
                    observed.append((batch, n))
                    configs.append(config)
                    return [dict(row, implementation=implementation, n=n, ngrams_batch_size=batch)
                            for _ in cases]

                with patch.object(runner, "RESULTS", Path(directory)), \
                        patch.object(runner, "load_cases", return_value=[self.case] * 600), \
                        patch.object(runner, "_ensure_binaries"), \
                        patch.object(runner, "_environment_metadata", return_value={}), \
                        patch.object(runner, "load_config", return_value=sweep_config), \
                        patch.object(runner, "_run_phase", side_effect=phase), \
                        contextlib.redirect_stdout(io.StringIO()):
                    runner.run_benchmark(1, case_limit=1 if count == 1 else None)
                self.assertEqual(observed, expected)
                self.assertEqual(len({id(config) for config in configs}), 30)
                self.assertEqual(original, runner.load_config())
                summary = json.loads((Path(directory) / "benchmark-summary.json").read_text())
                self.assertEqual(summary["n_configurations"], 6)
                self.assertEqual(summary["batch_configurations"], 5)
                self.assertEqual(summary["case_timeout_seconds"], 300)
                self.assertEqual(summary["configurations_per_case"], 30)
                self.assertEqual(summary["implementations_per_case"], 30)
                self.assertEqual(summary["total_case_runs"], count * 30)
                self.assertEqual(summary["base_cases"], count)
                self.assertEqual(list(summary["implementations"]), [runner.phase_label(b, n) for b, n in expected])
                for batch, n in expected:
                    group = summary["implementations"][runner.phase_label(batch, n)]
                    self.assertEqual((group["ngrams_batch_size"], group["n"]), (batch, n))
                with (Path(directory) / "benchmark.csv").open(newline="") as stream:
                    reader = csv.DictReader(stream)
                    self.assertEqual(reader.fieldnames, list(runner.RESULT_FIELDS))
                    rows = list(reader)
                    self.assertEqual(len(rows), count * 30)
                    self.assertEqual([(int(rows[i * count]["ngrams_batch_size"]), int(rows[i * count]["n"])) for i in range(30)], expected)


if __name__ == "__main__":
    unittest.main()
