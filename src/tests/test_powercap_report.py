import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROCESSOR = ROOT / "Utils" / "processPowercapCalibrationCSV.py"


@unittest.skipUnless(importlib.util.find_spec("pandas"), "pandas is not installed")
class PowercapReportTest(unittest.TestCase):
    def run_report(self, csv, *args):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "measurements.csv"
            path.write_text(csv, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(PROCESSOR), str(path), *args],
                text=True, capture_output=True,
            )

    def test_baseline_and_single_repetition(self):
        result = self.run_report(
            "Powercap,Time_ms,cpu.Package_J\n-1,100,2\n15,120,1.8\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("-1 (base)", result.stdout)
        self.assertIn("0.8333", result.stdout)
        self.assertIn("1.1111", result.stdout)

    def test_missing_and_invalid_energy_are_reported(self):
        for value in ("NOT_SUPPORTED", "0", "nan"):
            with self.subTest(value=value):
                result = self.run_report(
                    f"Powercap,Time_ms,cpu.Package_J\n-1,100,2\n15,120,{value}\n"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("finite positive measurements", result.stderr)
        result = self.run_report("Powercap,Time_ms\n-1,100\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not contain a CPU Package energy metric", result.stderr)

    def test_baseline_is_not_labeled_as_a_watt_limit(self):
        result = self.run_report(
            "Powercap,Time_ms,cpu.Package_J\n-1,100,1\n15,120,2\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("saved baseline", result.stdout)

    def test_accepts_an_explicit_duration_aspect_column(self):
        result = self.run_report(
            "Powercap,Time.WallClock_ms,cpu.Package_J\n-1,100,2\n15,120,1.8\n",
            "--time-column", "Time.WallClock_ms")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("0.8333", result.stdout)


if __name__ == "__main__":
    unittest.main()
