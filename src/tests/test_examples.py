import csv
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "src" / "tests" / "legacy-engine"
EXAMPLES = ROOT / "examples"


class ExamplesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "src"), "tests/legacy-engine"],
                       check=True, capture_output=True, text=True)
        subprocess.run(["make", "-C", str(EXAMPLES / "custom_metric")],
                       check=True, capture_output=True, text=True)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.backend_root = self.directory / "backends"
        self.backend_root.mkdir()
        self.environment = os.environ.copy()
        self.environment["GREENMEASURER_BACKEND_DIR"] = str(self.backend_root)
        self.calibration = self.directory / "calibration.txt"
        self.calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def test_executable_configuration_examples(self):
        cases = {
            "configurations/system-cores.toml": ["Cores"],
            "configurations/system-processes.toml": [
                "CPU_Memory_KB", "Workload_Processes", "System_Running_Processes"
            ],
            "configurations/external-processes.toml": ["external.processes.RunningProcesses_count"],
            "configurations/system-and-external.toml": [
                "Cores", "System_Running_Processes",
                "external.processes.RunningProcesses_count",
            ],
            "custom_metric/configuration.toml": [
                "System_Running_Processes", "external.processes.RunningProcesses_count",
            ],
        }
        for name, columns in cases.items():
            with self.subTest(configuration=name):
                output = self.directory / "result.csv"
                result = subprocess.run(
                    [str(MAIN), "--config", str(EXAMPLES / name),
                     "-command", "sleep 0.12", "-output", str(output),
                     "-calib-file", str(self.calibration)],
                    cwd=self.directory, env=self.environment, text=True,
                    capture_output=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows[0], ["Language", "Program", "Powercap",
                                           "Timestamp_ms", *columns])
                self.assertGreaterEqual(len(rows), 3)
                self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
                timestamps = [float(row[3]) for row in rows[1:]]
                self.assertTrue(all(a < b for a, b in zip(timestamps, timestamps[1:])))
                for index, row in enumerate(rows[1:], 1):
                    for column, value in zip(columns, row[4:]):
                        if value == "NOT_SUPPORTED":
                            self.assertIn(column, ("CPU_Memory_KB", "Workload_Processes"))
                            self.assertEqual(index, len(rows) - 1)
                        else:
                            self.assertGreaterEqual(float(value), 0)

    def test_toy_backend_examples_in_both_modes(self):
        for category in ("cpu", "gpu"):
            source = EXAMPLES / "toy_backends" / category / f"toy_{category}" / f"toy_{category}.c"
            plugin = self.backend_root / category / f"toy_{category}" / "backend.so"
            plugin.parent.mkdir(parents=True)
            subprocess.run(
                ["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                 "-I", str(ROOT / "src" / "include"), str(source), "-o", str(plugin)],
                check=True, capture_output=True, text=True,
            )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                output = self.directory / f"{mode}.csv"
                result = subprocess.run(
                    [str(MAIN), f"--{mode}", "-command", "sleep 0.15",
                     "-language", "test", "-program", "example", "-variance", "100",
                     "-sleep-secs", "0", "-powercap", "-1",
                     "-calib-file", str(self.calibration), "-output", str(output)],
                    cwd=self.directory, env=self.environment, text=True,
                    capture_output=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("TOY_CPU", result.stdout)
                self.assertIn("TOY_GPU", result.stdout)
                header_mode = "--csv-header" if mode == "standard" else "--overtime-csv-header"
                header = subprocess.run(
                    [str(MAIN), header_mode], cwd=self.directory, env=self.environment,
                    text=True, capture_output=True, check=True,
                ).stdout.strip()
                columns = next(csv.reader([header]))
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows.pop(0), columns)
                self.assertTrue(rows)
                for row in rows:
                    self.assertEqual(len(row), len(columns))
                    self.assertGreater(float(row[columns.index("cpu.Energy_J")]), 0)
                    self.assertGreater(float(row[columns.index("gpu.Energy_J")]), 0)
                    self.assertEqual(float(row[columns.index("gpu.Power_W")]), 5.0)

        output = self.directory / "configured.csv"
        result = subprocess.run(
            [str(MAIN), "--config", str(EXAMPLES / "configurations" / "toy-cpu-gpu.toml"),
             "-command", "sleep 0.15", "-output", str(output), "-variance", "100",
             "-calib-file", str(self.calibration)],
            cwd=self.directory, env=self.environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Timestamp_ms",
                                   "Cores", "cpu.Energy_J", "gpu.Energy_J", "gpu.Power_W"])
        self.assertGreaterEqual(len(rows), 3)
        for row in rows[1:]:
            self.assertEqual(len(row), len(rows[0]))
            self.assertGreater(float(row[5]), 0)
            self.assertGreater(float(row[6]), 0)
            self.assertEqual(float(row[7]), 5.0)

        output = self.directory / "configured-standard.csv"
        result = subprocess.run(
            [str(MAIN), "--config", str(EXAMPLES / "configurations" / "standard-toy-cpu-gpu.toml"),
             "-command", "sleep 0.17", "-output", str(output), "-variance", "100",
             "-calib-file", str(self.calibration)],
            cwd=self.directory, env=self.environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms",
                                   "Cores", "cpu.Energy_J", "gpu.Energy_J", "gpu.Power_W"])
        self.assertEqual(len(rows), 3)
        for row in rows[1:]:
            self.assertEqual(len(row), len(rows[0]))
            elapsed = float(row[3]) / 1000
            self.assertAlmostEqual(float(row[5]), elapsed * 10, delta=0.1)
            self.assertAlmostEqual(float(row[6]), elapsed * 5, delta=0.05)
            self.assertEqual(float(row[7]), 5)

    def test_standard_configuration_example(self):
        output = self.directory / "standard-example.csv"
        result = subprocess.run(
            [str(MAIN), "--config", str(EXAMPLES / "configurations" /
                                        "standard-system-and-external.toml"),
             "-command", "sleep 0.02", "-output", str(output), "-variance", "100",
             "-calib-file", str(self.calibration)],
            cwd=self.directory, env=self.environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms", "Cores",
                                   "external.processes.RunningProcesses_count"])
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(len(row) == len(rows[0]) and float(row[3]) > 0 and
                            int(row[4]) > 0 and float(row[5]) >= 0 for row in rows[1:]))


if __name__ == "__main__":
    unittest.main()
