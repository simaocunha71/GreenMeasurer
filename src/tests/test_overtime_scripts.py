import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ANALYZE = ROOT / "scripts" / "analyze_overtime.py"
COMPARE = ROOT / "scripts" / "compare_overtime_overhead.py"
PROBE = ROOT / "scripts" / "probe_rapl_frequency.py"


class OvertimeScriptsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_script(self, script, *args, env=None):
        return subprocess.run(
            [sys.executable, str(script), *map(str, args)],
            text=True, capture_output=True, cwd=ROOT, env=env,
        )

    def test_analysis_statistics_and_run_boundaries(self):
        source = self.directory / "samples.csv"
        source.write_text(
            "Language,Program,Powercap,Timestamp_ms\n"
            "C,test,-1,1\nC,test,-1,2\nC,test,-1,5\n"
            "C,test,-1,0.5\nC,test,-1,2.5\nC,test,5,4\n",
            encoding="utf-8",
        )
        result = self.run_script(ANALYZE, source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Samples: 3; first=1.000000 ms; last=5.000000 ms", result.stdout)
        self.assertIn("mean=2.000000 ms; median=2.000000 ms", result.stdout)
        self.assertIn("p95=2.900000 ms", result.stdout)
        self.assertIn("Above 1 ms: 1/2 (50.00%)", result.stdout)
        self.assertIn("Run 2:", result.stdout)
        self.assertIn("Run 3:", result.stdout)
        self.assertIn("No consecutive sample intervals available", result.stdout)

    def test_analysis_rejects_legacy_and_invalid_data(self):
        source = self.directory / "samples.csv"
        source.write_text("Language,Program,Powercap,Time_ms\nC,test,-1,3\n", encoding="utf-8")
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Timestamp_ms", result.stderr)
        source.write_text("Language,Program,Powercap,Timestamp_ms\nC,test,-1,nan\n", encoding="utf-8")
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("finite", result.stderr)

    def test_probe_rejects_invalid_duration(self):
        result = self.run_script(PROBE, "--duration", "nan")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("finite and positive", result.stderr)

    def test_compare_with_toy_backend(self):
        executable = ROOT / "src" / "main"
        self.assertTrue(executable.is_file(), "build src/main before running the tests")
        backend_dir = self.directory / "backends" / "cpu" / "toy"
        backend_dir.mkdir(parents=True)
        subprocess.run(
            ["cc", "-O2", "-fPIC", "-shared", "-o", str(backend_dir / "backend.so"),
             str(ROOT / "examples" / "toy_backends" / "cpu" / "toy_cpu" / "toy_cpu.c")],
            check=True,
        )
        calibration = self.directory / "calib.txt"
        calibration.write_text("0\n", encoding="utf-8")
        env = os.environ.copy()
        env["GREENMEASURER_BACKEND_DIR"] = str(self.directory / "backends")
        result = self.run_script(
            COMPARE, "--main", executable, "--calib-file", calibration,
            "--command", "sleep 0.03", "--runs", "2", "--interval-us", "5000", env=env,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(",baseline,"), 2)
        self.assertEqual(result.stdout.count(",overtime,"), 2)
        self.assertIn("Mean difference:", result.stdout)
        self.assertIn("does not measure energy overhead", result.stdout)

        failure = self.run_script(
            COMPARE, "--main", executable, "--calib-file", calibration,
            "--command", "exit 4", "--runs", "1", env=env,
        )
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("status", failure.stderr)


if __name__ == "__main__":
    unittest.main()
