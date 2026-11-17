import os
from pathlib import Path

from temperature_fixture import install_temperature_backend
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
            "Language,Program,Powercap,Timestamp_us\n"
            "C,test,-1,1000\nC,test,-1,2000\nC,test,-1,5000\n"
            "C,test,-1,500\nC,test,-1,2500\nC,test,5,4000\n",
            encoding="utf-8",
        )
        result = self.run_script(ANALYZE, source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Samples: 3; first=1000.000000 us; last=5000.000000 us", result.stdout)
        self.assertIn("mean=2000.000000 us; median=2000.000000 us", result.stdout)
        self.assertIn("p95=2900.000000 us", result.stdout)
        self.assertIn("Above 1000 us: 1/2 (50.00%)", result.stdout)
        self.assertIn("Run 2:", result.stdout)
        self.assertIn("Run 3:", result.stdout)
        self.assertIn("No consecutive sample intervals available", result.stdout)
        result = self.run_script(ANALYZE, source, "--threshold-us", 3000)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Above 3000 us: 0/2", result.stdout)

    def test_analysis_rejects_invalid_data(self):
        source = self.directory / "samples.csv"
        source.write_text("Language,Program,Powercap,Time_ms\nC,test,-1,3\n", encoding="utf-8")
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Timestamp_us", result.stderr)
        source.write_text("Language,Program,Powercap,Timestamp_us\nC,test,-1,nan\n", encoding="utf-8")
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("finite", result.stderr)

    def test_analysis_supports_legacy_millisecond_csv_and_threshold(self):
        source = self.directory / "legacy.csv"
        source.write_text(
            "Language,Program,Powercap,Timestamp_ms\nC,test,-1,1\nC,test,-1,3\n",
            encoding="utf-8",
        )
        result = self.run_script(ANALYZE, source, "--threshold-ms", 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("mean=2000.000000 us", result.stdout)
        self.assertIn("Above 1000 us: 1/1", result.stdout)
        source.write_text(
            "Language,Program,Powercap,Timestamp_ms,Timestamp_us\nC,test,-1,1,1000\n",
            encoding="utf-8",
        )
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one", result.stderr)

    def test_probe_rejects_invalid_duration(self):
        result = self.run_script(PROBE, "--duration", "nan")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("finite and positive", result.stderr)

    def test_compare_with_toy_backend(self):
        backend_dir = self.directory / "backends" / "cpu" / "toy"
        backend_dir.mkdir(parents=True)
        subprocess.run(
            ["cc", "-O2", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
             "-o", str(backend_dir / "backend.so"),
             str(ROOT / "src" / "tests" / "fixtures" / "toy_backends" / "cpu" / "toy_cpu" / "toy_cpu.c")],
            check=True,
        )
        calibration = self.directory / "calib.txt"
        calibration.write_text("0\n", encoding="utf-8")
        output = self.directory / "user-results.csv"
        output.write_text("preserved\n", encoding="utf-8")
        config = self.directory / "config.toml"
        config.write_text(
            '[measurement]\nmode="over-time"\ncommand="sleep 0.03"\n'
            f'calibration_file="{calibration}"\noutput="{output}"\n'
            'variance=100\nsleep_seconds=0\ninterval_us=5000\n'
            '[metrics]\nselect=["cpu.Energy"]\n', encoding="utf-8",
        )
        env = os.environ.copy()
        env["GREENMEASURER_BACKEND_DIR"] = str(self.directory / "backends")
        result = self.run_script(
            COMPARE, "--config", config, "--runs", "2", env=env,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(",baseline,"), 2)
        self.assertEqual(result.stdout.count(",overtime,"), 2)
        for line in result.stdout.splitlines():
            if ",overtime," in line:
                fields = line.split(",")
                self.assertGreater(float(fields[3]), 10)
                self.assertLess(float(fields[3]), float(fields[4]) + 50)
        self.assertIn("Mean difference:", result.stdout)
        self.assertIn("does not measure energy overhead", result.stdout)
        self.assertEqual(output.read_text(encoding="utf-8"), "preserved\n")

        config.write_text(config.read_text(encoding="utf-8").replace(
            'select=["cpu.Energy"]', 'select="all"'), encoding="utf-8")
        result = self.run_script(COMPARE, "--config", config, "--runs", "1", env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(",overtime,"), 1)
        self.assertEqual(output.read_text(encoding="utf-8"), "preserved\n")

        config.write_text(config.read_text(encoding="utf-8").replace("sleep 0.03", "exit 4"),
                          encoding="utf-8")
        failure = self.run_script(
            COMPARE, "--config", config, "--runs", "1", env=env,
        )
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("status", failure.stderr)

    def test_compare_requires_valid_overtime_configuration(self):
        config = self.directory / "config.toml"
        calibration = self.directory / "calib.txt"
        calibration.write_text("0\n", encoding="utf-8")
        config.write_text(
            '[measurement]\nmode="standard"\ncommand="true"\n'
            f'calibration_file="{calibration}"\n'
            '[metrics]\nselect=["system.cores"]\n', encoding="utf-8",
        )
        result = self.run_script(COMPARE, "--config", config)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("measurement.mode must be 'over-time'", result.stderr)
        result = self.run_script(COMPARE, "--command", "true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--config", result.stderr)
        config.write_text(
            '[measurement]\nmode="over-time"\ncommand="true"\npowercap=5\n'
            f'calibration_file="{calibration}"\n'
            '[metrics]\nselect=["system.cores"]\n', encoding="utf-8",
        )
        result = self.run_script(COMPARE, "--config", config)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("powercap must be -1", result.stderr)

    def test_compare_builds_source_metric_using_config_relative_paths(self):
        calibration = self.directory / "calib.txt"
        calibration.write_text("0\n", encoding="utf-8")
        (self.directory / "metric.h").write_text("int metric(void);\n", encoding="utf-8")
        (self.directory / "metric.c").write_text(
            '#include "metric.h"\nint metric(void) { return 42; }\n', encoding="utf-8",
        )
        backends = self.directory / "backends"
        backends.mkdir()
        install_temperature_backend(backends)
        env = os.environ.copy()
        env["GREENMEASURER_BACKEND_DIR"] = str(backends)
        config = self.directory / "config.toml"
        config.write_text(
            '[measurement]\nmode="over-time"\ncommand="sleep 0.03"\n'
            f'calibration_file="{calibration}"\n'
            'variance=100\nsleep_seconds=0\ninterval_us=5000\n'
            '[metrics]\nselect=["external.demo.Value"]\n'
            '[[external_metrics]]\nname="demo"\nmetric="Value"\nunit="count"\n'
            'function="metric"\nheader="metric.h"\nsources=["metric.c"]\n'
            'return_type="int"\n', encoding="utf-8",
        )
        result = self.run_script(COMPARE, "--config", config, "--runs", "1", env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Built external metric", result.stderr)
        self.assertEqual(result.stdout.count(",overtime,"), 1)


if __name__ == "__main__":
    unittest.main()
