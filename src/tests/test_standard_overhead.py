import csv
from contextlib import redirect_stderr
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from temperature_fixture import install_temperature_backend


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "compare_standard_overhead.py"
sys.path.insert(0, str(ROOT / "scripts"))
from compare_standard_overhead import read_measurement, report_summary, run_timed


class StandardOverheadTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.backends = self.directory / "backends"
        self.backends.mkdir()
        install_temperature_backend(self.backends)
        self.calibration = self.directory / "calibration.txt"
        self.calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        self.output = self.directory / "user.csv"
        self.output.write_text("preserved\n", encoding="utf-8")
        self.config = self.directory / "config.toml"
        self.config.write_text(
            '[measurement]\nmode="standard"\ncommand="sleep 0.03"\n'
            f'calibration_file="{self.calibration}"\noutput="{self.output}"\n'
            'variance=100\nsleep_seconds=0\nrepetitions=3\npowercap=[-1, 5]\n'
            '[metrics]\nselect="all"\n', encoding="utf-8",
        )
        self.environment = os.environ.copy()
        self.environment["GREENMEASURER_BACKEND_DIR"] = str(self.backends)

    def tearDown(self):
        self.temporary.cleanup()

    def run_script(self, *args):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--config", str(self.config), *args],
            cwd=ROOT, env=self.environment, text=True, capture_output=True, timeout=120,
        )

    def test_progressive_metrics_produce_raw_rounds_without_changing_user_output(self):
        result = self.run_script(
            "--metrics", "system.cores", "system.workload_processes",
            "--runs", "2", "--seed", "7",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        self.assertEqual(len(rows), 6)
        self.assertEqual([int(row["order"]) for row in rows], list(range(1, 7)))
        for round_number in ("1", "2"):
            group = [row for row in rows if row["round"] == round_number]
            self.assertEqual({row["metrics_count"] for row in group}, {"0", "1", "2"})
            for row in group:
                self.assertGreater(float(row["workload_ms"]), 10)
                self.assertGreater(float(row["total_ms"]), 10)
                self.assertEqual(row["thermal_wait"], "0")
            full = next(row for row in group if row["metrics_count"] == "2")
            self.assertEqual(full["metrics"], "system.cores;system.workload_processes")
        self.assertIn("powercap=-1", result.stderr)
        self.assertIn("2 metrics: total median=", result.stderr)
        self.assertIn("Energy overhead is not measured", result.stderr)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "preserved\n")

    def test_rejects_missing_metric_without_success_output(self):
        result = self.run_script(
            "--metrics", "system.cores", "cpu.Missing", "--runs", "1",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cpu.Missing", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(self.output.read_text(encoding="utf-8"), "preserved\n")

    def test_rejects_duplicate_metrics(self):
        result = self.run_script("--metrics", "system.cores", "system.cores")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicates", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_rejects_unavailable_metric_value(self):
        source = self.directory / "result.csv"
        source.write_text("Time_ms,Cores,Workload_Processes_Peak\n"
                          "20,4,NOT_SUPPORTED\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "could not be read"):
            read_measurement(source, ["system.cores", "system.workload_processes"], [])

    def test_marks_temperature_waits(self):
        elapsed, waited = run_timed([
            "/bin/sh", "-c", "printf '[INFO] Waiting for temperature (Standard)\\n'",
        ])
        self.assertGreater(elapsed, 0)
        self.assertTrue(waited)

    def test_summary_compares_total_and_workload_medians_to_baseline(self):
        records = [
            {"metrics_count": 0, "total_ms": "100", "workload_ms": "100"},
            {"metrics_count": 0, "total_ms": "200", "workload_ms": "200"},
            {"metrics_count": 1, "total_ms": "165", "workload_ms": "151"},
            {"metrics_count": 1, "total_ms": "195", "workload_ms": "179"},
        ]
        output = io.StringIO()
        with redirect_stderr(output):
            report_summary(records, 1)
        self.assertIn("total median=180.000000", output.getvalue())
        self.assertIn("total difference=20.00%", output.getvalue())
        self.assertIn("workload difference=10.00%", output.getvalue())


if __name__ == "__main__":
    unittest.main()
