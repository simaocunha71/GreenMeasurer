import csv
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from temperature_fixture import install_temperature_backend


ROOT = Path(__file__).resolve().parents[2]


class SystemFailureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "src"), "all", "tests/legacy-engine.o"], check=True,
                       capture_output=True, text=True)
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        cls.binary = cls.directory / "main"
        flags = subprocess.run(
            ["make", "-s", "--no-print-directory", "-C", str(ROOT / "src"),
             "print-configured-host-libs"], check=True, capture_output=True, text=True,
        ).stdout
        subprocess.run(
            ["cc", "-pthread", "-rdynamic", str(ROOT / "src" / "tests" / "legacy-engine.o"),
             str(ROOT / "src" / "utils" / "memory.o"),
             str(ROOT / "src" / "metrics" / "system_metrics.o"),
             str(ROOT / "src" / "metrics" / "metric_adapter_loader.o"),
             str(ROOT / "src" / "tests" / "system_metric_wrappers.c"),
             "-Wl,--wrap=sys_core_count", "-Wl,--wrap=sys_running_processes",
             "-Wl,--wrap=sys_core_frequency_mhz",
             "-Wl,--wrap=sys_workload_process_count", "-Wl,--wrap=sys_workload_memory_kb",
             "-o", str(cls.binary),
             *shlex.split(flags), "-pthread", "-lm", "-ldl"],
            check=True, capture_output=True, text=True,
        )
        cls.backends = cls.directory / "backends"
        cls.backends.mkdir()
        install_temperature_backend(cls.backends)
        cls.calibration = cls.directory / "calibration"
        cls.calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def measure(self, mode, failure, metrics, repetitions=1):
        output = self.directory / f"{mode}-{failure}.csv"
        env = os.environ.copy()
        env["GREENMEASURER_BACKEND_DIR"] = str(self.backends)
        env["GREEN_TEST_SYSTEM_FAILURE"] = failure
        args = [str(self.binary), f"--configured-{mode}", "-command", "sleep 0.18",
                "-output", str(output), "-calib-file", str(self.calibration),
                "-variance", "100", "-sleep-secs", "0"]
        if mode == "overtime":
            args.extend(["-interval-us", "10000"])
        elif repetitions > 1:
            args.extend(["-n_times", str(repetitions)])
        for metric in metrics:
            args.extend(["--configured-system", metric])
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=15)
        rows = []
        if output.exists():
            with output.open(newline="", encoding="utf-8") as file:
                rows = list(csv.reader(file, skipinitialspace=True))
            output.unlink()
        return result, rows

    def test_preflight_skips_unavailable_system_metric(self):
        for mode in ("standard", "overtime"):
            for failure, metric in (("frequency-preflight", "system.core_frequency"),
                                    ("running-preflight", "system.running_processes")):
                with self.subTest(mode=mode, failure=failure):
                    result, rows = self.measure(mode, failure, ["system.cores", metric])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f"Skipping unavailable system metric '{metric}'", result.stderr)
                    self.assertEqual(rows[0][-1], "Cores")
                    self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
                    result, rows = self.measure(mode, failure, [metric])
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("No selected metrics are available", result.stderr)
                    self.assertFalse(rows)

    def test_failed_runtime_samples_preserve_schema_and_invalidate_aggregates(self):
        for mode in ("standard", "overtime"):
            for failure, metric in (("frequency-runtime", "system.core_frequency"),
                                    ("running-runtime", "system.running_processes")):
                with self.subTest(mode=mode, failure=failure):
                    result, rows = self.measure(mode, failure, ["system.cores", metric])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f"Failed to read {metric}", result.stderr)
                    self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
                    if failure == "frequency-runtime":
                        self.assertEqual(rows[-1][-2], "NOT_SUPPORTED")
                        self.assertEqual(float(rows[-1][-1]), 1001)
                        if mode == "overtime":
                            self.assertEqual(float(rows[1][-2]), 1000)
                    else:
                        self.assertEqual(rows[-1][-1], "NOT_SUPPORTED")
                        if mode == "overtime":
                            self.assertEqual(float(rows[1][-1]), 7)

    def test_core_count_unavailable_at_preflight(self):
        for mode in ("standard", "overtime"):
            result, rows = self.measure(mode, "cores-preflight",
                                        ["system.cores", "system.running_processes"])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Skipping unavailable system metric 'system.cores'", result.stderr)
            self.assertTrue(all("Cores" not in name for name in rows[0]))
            result, rows = self.measure(mode, "cores-preflight", ["system.cores"])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("No selected metrics are available", result.stderr)
            self.assertFalse(rows)

    def test_process_and_core_failures_are_marked(self):
        cases = (("standard", "cores-runtime", "system.cores"),
                 ("overtime", "cores-runtime", "system.cores"),
                 ("standard", "workload-runtime", "system.workload_processes"),
                 ("overtime", "workload-runtime", "system.workload_processes"),
                 ("overtime", "memory-runtime", "system.memory"))
        for mode, failure, metric in cases:
            with self.subTest(mode=mode, failure=failure):
                result, rows = self.measure(mode, failure, [metric])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"Failed to read {metric}", result.stderr)
                self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
                self.assertIn("NOT_SUPPORTED", [row[-1] for row in rows[1:]])

    def test_failed_aggregate_recovers_on_next_repetition(self):
        for failure, metric in (("frequency-transient", "system.core_frequency"),
                                ("running-transient", "system.running_processes")):
            with self.subTest(failure=failure):
                result, rows = self.measure("standard", failure, [metric], repetitions=2)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(rows), 3)
                column = -2 if failure == "frequency-transient" else -1
                self.assertEqual(rows[1][column], "NOT_SUPPORTED")
                self.assertGreater(float(rows[2][column]), 0)


if __name__ == "__main__":
    unittest.main()
