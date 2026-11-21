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
            "Program,Powercap,Timestamp_us\n"
            "test,-1,1000\ntest,-1,2000\ntest,-1,5000\n"
            "test,-1,500\ntest,-1,2500\ntest,5,4000\n",
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
        source.write_text("Program,Powercap,Time_ms\ntest,-1,3\n", encoding="utf-8")
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Timestamp_us", result.stderr)
        source.write_text("Program,Powercap,Timestamp_us\ntest,-1,nan\n", encoding="utf-8")
        result = self.run_script(ANALYZE, source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("finite", result.stderr)

    def test_analysis_supports_legacy_millisecond_csv_and_threshold(self):
        source = self.directory / "legacy.csv"
        source.write_text(
            "Program,Powercap,Timestamp_ms\ntest,-1,1\ntest,-1,3\n",
            encoding="utf-8",
        )
        result = self.run_script(ANALYZE, source, "--threshold-ms", 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("mean=2000.000000 us", result.stdout)
        self.assertIn("Above 1000 us: 1/1", result.stdout)
        source.write_text(
            "Program,Powercap,Timestamp_ms,Timestamp_us\ntest,-1,1,1000\n",
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
        (self.directory / "metric.h").write_text("int metric(void);\n", encoding="utf-8")
        (self.directory / "metric.c").write_text("int metric(void) { return 42; }\n",
                                                  encoding="utf-8")
        output = self.directory / "user-results.csv"
        output.write_text("preserved\n", encoding="utf-8")
        config = self.directory / "config.toml"
        config.write_text(
            '[measurement]\naspect="Toy"\nmode="over-time"\n\n'
            '[device]\nheader="metric.h"\nlib="metric.c"\n\n'
            '[[metric]]\nname="Toy.Value"\nmeasurement="int metric()"\n', encoding="utf-8",
        )
        result = self.run_script(
            COMPARE, "--config", config, "--command", "sleep 0.03", "--metrics", "Toy.Value",
            "--interval-ms", "5", "--runs", "2",
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

        failure = self.run_script(
            COMPARE, "--config", config, "--command", "exit 4", "--metrics", "Toy.Value",
            "--interval-ms", "5", "--runs", "1",
        )
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("status", failure.stderr)

    def test_compare_requires_valid_overtime_configuration(self):
        config = self.directory / "config.toml"
        (self.directory / "metric.h").write_text("int metric(void);\n", encoding="utf-8")
        config.write_text(
            '[measurement]\naspect="Demo"\nmode="aggregate"\n\n'
            '[device]\nheader="metric.h"\n\n'
            '[[metric]]\nname="Demo.Value"\nmeasurement="int metric()"\n', encoding="utf-8",
        )
        result = self.run_script(COMPARE, "--config", config, "--command", "true",
                                 "--metrics", "Demo.Value", "--interval-ms", "5")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mode = "over-time"', result.stderr)
        result = self.run_script(COMPARE, "--command", "true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--config", result.stderr)
        config.write_text(
            '[measurement]\naspect="Demo"\nmode="over-time"\n\n'
            '[device]\nheader="metric.h"\n\n'
            '[[metric]]\nname="Demo.Value"\nmeasurement="int metric()"\n', encoding="utf-8",
        )
        result = self.run_script(COMPARE, "--config", config, "--command", "true",
                                 "--metrics", "Missing", "--interval-ms", "5")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing", result.stderr)

    def test_compare_builds_source_metric_using_config_relative_paths(self):
        (self.directory / "metric.h").write_text("int metric(void);\n", encoding="utf-8")
        (self.directory / "metric.c").write_text(
            '#include "metric.h"\nint metric(void) { return 42; }\n', encoding="utf-8",
        )
        config = self.directory / "config.toml"
        config.write_text(
            '[measurement]\naspect="Demo"\nmode="over-time"\n\n'
            '[device]\nheader="metric.h"\nlib="metric.c"\n\n'
            '[[metric]]\nname="Demo.Value"\nmeasurement="int metric()"\nunit="count"\n',
            encoding="utf-8",
        )
        result = self.run_script(COMPARE, "--config", config, "--command", "sleep 0.03",
                                 "--metrics", "Demo.Value", "--interval-ms", "5", "--runs", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(",overtime,"), 1)

    def test_comparisons_control_an_unmeasured_temperature(self):
        fixture = ROOT / "src/tests/fixtures/weaver"
        common = f'[device]\nheader="{fixture / "toy.h"}"\nlib="{fixture / "toy.c"}"\n'
        baseline = self.directory / "cpu.txt"
        baseline.write_text("Average temperature: 40.0 C\n", encoding="utf-8")
        configs = {}
        for mode in ("aggregate", "over-time"):
            time_config = self.directory / f"thermal-time-{mode}.toml"
            time_config.write_text(
                f'[measurement]\naspect="Time"\nmode="{mode}"\n'
                'measurement_start="int gm_wall_clock_time_start()"\n'
                'measurement_stop="int gm_wall_clock_time_stop()"\n\n'
                f'[device]\nheader="{ROOT / "src/aspects/wall_clock_time.h"}"\n'
                f'lib="{ROOT / "src/aspects/wall_clock_time.c"}"\n\n'
                '[[metric]]\nname="Time.WallClock"\n'
                'measurement="double gm_wall_clock_time_read()"\n'
                'kind="duration"\nunit="ms"\n', encoding="utf-8")
            cpu_config = self.directory / f"thermal-cpu-{mode}.toml"
            cpu_config.write_text(
                f'[measurement]\naspect="CPU.Temperature"\nmode="{mode}"\n\n' + common +
                '\n[[metric]]\nname="CPU.Temperature"\nmeasurement="double toy_thermal()"\n'
                'thermal=true\ncalibration="double toy_calibrate_thermal(unsigned)"\n', encoding="utf-8")
            gpu_config = self.directory / f"thermal-gpu-{mode}.toml"
            gpu_config.write_text(
                f'[measurement]\naspect="GPU.Temperature"\nmode="{mode}"\n\n' + common +
                '\n[[metric]]\nname="GPU.Temperature"\nmeasurement="float toy_temperature()"\n'
                'thermal=true\ncalibration="float toy_calibrate_temperature(unsigned)"\n', encoding="utf-8")
            configs[mode] = [time_config, cpu_config, gpu_config]
        for script, mode, extra in ((COMPARE, "over-time", ["--interval-ms", "5"]),
                                    (ROOT / "scripts/compare_aggregate_overhead.py", "aggregate",
                                     ["--time-aspect", "Time.WallClock"])):
            with self.subTest(script=script):
                result = self.run_script(
                    script, "--config", *configs[mode], "--command", "sleep 0.02",
                    "--metrics", "GPU.Temperature", "--runs", "1",
                    "--temperature-control", "CPU.Temperature",
                    "--calib-file", f"CPU.Temperature={baseline}", "--variance", "5", *extra)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
