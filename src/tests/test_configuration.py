import csv
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from temperature_fixture import install_temperature_backend


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "scripts" / "validate_config.py"


class ConfigurationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(
            ["make", "-C", str(ROOT / "src"), "tests/legacy-engine", "metric-adapter-check"],
            check=True, capture_output=True, text=True,
        )

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.file = self.directory / "config.toml"

    def tearDown(self):
        self.temporary.cleanup()

    def validate(self, text, *options):
        self.file.write_text(text, encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(VALIDATOR), str(self.file), *options],
            text=True, capture_output=True,
        )

    def build_adapter(self, source, output="adapter with spaces.so"):
        path = self.directory / output
        subprocess.run(
            ["cc", "-fPIC", "-shared", str(ROOT / "src" / "tests" / source),
             "-o", str(path)], check=True, capture_output=True, text=True,
        )
        return path

    def adapter_config(self, path, selected):
        return (
            '[measurement]\nmode="standard"\n'
            f'[metrics]\nselect={selected}\n'
            f'[[adapters]]\nname="demo"\npath="{path.name}"\n'
        )

    def run_configured(self, text, *options, env=None, cwd=None):
        self.file.write_text(text, encoding="utf-8")
        environment = os.environ.copy()
        backends = self.directory / "backends"
        backends.mkdir(exist_ok=True)
        install_temperature_backend(backends)
        environment["GREENMEASURER_BACKEND_DIR"] = str(backends)
        if env:
            environment.update(env)
        return subprocess.run(
            [str(ROOT / "src" / "tests" / "legacy-engine"), "--config", str(self.file), *options],
            text=True, capture_output=True, env=environment, cwd=cwd, timeout=15,
        )

    def overtime_config(self, path, selected):
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        return (
            '[measurement]\nmode="over-time"\ncommand="sleep 0.06"\n'
            f'language="test"\nprogram="fixture"\noutput="{self.directory / "out.csv"}"\n'
            f'calibration_file="{calibration}"\ninterval_us=10000\nsleep_seconds=0\n'
            f'[metrics]\nselect={selected}\n'
            f'[[adapters]]\nname="demo"\npath="{path.name}"\n'
        )

    def test_overtime_config_executes_selected_metrics(self):
        path = self.build_adapter("configured_metric_adapter.c")
        result = self.run_configured(self.overtime_config(
            path, '["system.running_processes", "external.demo.Negative", "external.demo.Sequence"]'
        ))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No GPU energy backend active", result.stderr)
        with (self.directory / "out.csv").open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Timestamp_us",
                                  "System_Running_Processes", "external.demo.Negative_units",
                                  "external.demo.Sequence_count"])
        self.assertGreaterEqual(len(rows), 3)
        self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
        self.assertTrue(all(float(row[5]) == -5 for row in rows[1:]))
        self.assertTrue(all(float(a[3]) < float(b[3]) for a, b in zip(rows[1:], rows[2:])))
        self.assertTrue(all(float(a[6]) < float(b[6]) for a, b in zip(rows[1:], rows[2:])))

    def test_overtime_records_counter_snapshots(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        source = ROOT / "src" / "tests" / "fake_backend.c"
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "counter.csv"
        config = (
            '[measurement]\nmode="over-time"\ncommand="sleep 0.06"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\n'
            'sleep_seconds=0\nvariance=100\ninterval_us=5000\n'
            '[metrics]\nselect=["cpu.Package"]\n'
        )
        for flag in ("FAKE_COUNTER_SEQUENCE", "FAKE_NO_COUNTER"):
            with self.subTest(flag=flag):
                subprocess.run(
                    ["cc", "-fPIC", "-shared", '-DFAKE_NAME="Counter"',
                     "-DFAKE_PRIORITY=100", "-DFAKE_DETECT=1", f"-D{flag}",
                     str(source), "-o", str(plugin)],
                    check=True, capture_output=True, text=True,
                )
                result = self.run_configured(config)
                self.assertEqual(result.returncode, 0, result.stderr)
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                values = [row[-1] for row in rows[1:]]
                self.assertGreaterEqual(len(values), 3)
                if flag == "FAKE_COUNTER_SEQUENCE":
                    self.assertEqual([float(value) for value in values[:3]], [100, 100, 101])
                else:
                    self.assertTrue(all(value == "NOT_SUPPORTED" for value in values))
                    self.assertIn("does not expose counter readings", result.stderr)
                output.unlink()

    def test_overtime_external_metric_limit(self):
        source = self.directory / "many_metrics.c"
        adapter = self.directory / "many_metrics.so"
        definitions = ",\n".join(f'    {{"Value{i}", "count"}}' for i in range(31))
        source.write_text(
            '#include "metric-adapter.h"\n'
            f'static const GreenMetricDescriptor metrics[] = {{\n{definitions}\n}};\n'
            'static int read_metric(size_t index, double *value) '
            '{ *value = (double)index; return 0; }\n'
            'const GreenMetricAdapter greenmeasurer_metric_adapter = {\n'
            '    .abi_version = GREENMEASURER_METRIC_ADAPTER_ABI,\n'
            '    .metrics = metrics, .metric_count = 31, .read = read_metric\n'
            '};\n',
            encoding="utf-8",
        )
        subprocess.run(
            ["cc", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
             str(source), "-o", str(adapter)],
            check=True, capture_output=True, text=True,
        )

        def selection(count):
            return "[" + ", ".join(f'"external.demo.Value{i}"' for i in range(count)) + "]"

        result = self.run_configured(self.overtime_config(adapter, selection(30)))
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.directory / "out.csv"
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertGreaterEqual(len(rows), 2)
        self.assertEqual(rows[0][-1], "external.demo.Value29_count")
        self.assertTrue(all([float(value) for value in row[-30:]] == list(range(30))
                            for row in rows[1:]))
        output.unlink()

        marker = self.directory / "workload-started"
        config = self.overtime_config(adapter, selection(31)).replace(
            'command="sleep 0.06"', f'command="touch {marker}"'
        )
        result = self.run_configured(config)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Too many external metrics for over-time sampling (max 30)", result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(output.exists())

    def test_overtime_cli_overrides_config(self):
        path = self.build_adapter("configured_metric_adapter.c")
        override = self.directory / "override.csv"
        result = self.run_configured(
            self.overtime_config(path, '["external.demo.Negative"]'),
            "-command", "sleep 0.03", "-language", "override", "-output", str(override),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with override.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[1][0], "override")
        self.assertFalse((self.directory / "out.csv").exists())

    def test_overtime_external_runtime_failure_marks_sample(self):
        path = self.build_adapter("configured_metric_adapter.c")
        result = self.run_configured(
            self.overtime_config(path, '["external.demo.Sequence"]'),
            env={"GREEN_TEST_FAIL_AFTER": "2"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Failed to read external metric", result.stderr)
        self.assertIn("external.demo.Sequence", result.stderr)
        self.assertIn("check the adapter, library and runtime permissions", result.stderr)
        self.assertIn("library's .c and .h files", result.stderr)
        with (self.directory / "out.csv").open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertIn("NOT_SUPPORTED", [row[-1] for row in rows[1:]])

    def test_missing_backend_metric_is_skipped_when_another_is_available(self):
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        for mode in ("standard", "over-time"):
            with self.subTest(mode=mode):
                output = self.directory / f"missing-{mode}.csv"
                result = self.run_configured(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.02"\n'
                    f'output="{output}"\ncalibration_file="{calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\n'
                    'select=["system.cores", "cpu.NotProvided"]\n'
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("cpu.NotProvided", result.stderr)
                self.assertIn("skipping it", result.stderr)
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows[0][-1], "Cores")

    def test_invalid_backend_function_map_is_rejected(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="Fake"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_INVALID_FUNCTION_MAP",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        result = self.run_configured(
            '[measurement]\nmode="over-time"\ncommand="true"\n'
            f'output="{self.directory / "invalid.csv"}"\ncalibration_file="{calibration}"\n'
            'sleep_seconds=0\n[metrics]\nselect=["cpu.Package"]\n'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid metric function map", result.stdout)
        self.assertIn("No selected metrics are available", result.stderr)

    def test_short_name_and_function_alias_cannot_select_same_metric_twice(self):
        plugin = self.directory / "backends" / "cpu" / "toy" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
             str(ROOT / "src" / "tests" / "fixtures" / "toy_backends" / "cpu" / "toy_cpu" / "toy_cpu.c"),
             "-o", str(plugin)], check=True, capture_output=True, text=True,
        )
        result = self.run_configured(
            '[measurement]\nmode="over-time"\ncommand="true"\n[metrics]\n'
            'select=["cpu.Energy", "cpu.toy_cpu_metric_energy"]\n'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicated", result.stderr)

    def test_external_preflight_skips_unavailable_metric_in_both_modes(self):
        path = self.build_adapter("configured_metric_adapter.c")
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        for mode in ("standard", "over-time"):
            for other_metric in (True, False):
                with self.subTest(mode=mode, other_metric=other_metric):
                    output = self.directory / f"{mode}-{other_metric}.csv"
                    selection = '"system.cores", ' if other_metric else ""
                    config = (
                        f'[measurement]\nmode="{mode}"\ncommand="sleep 0.02"\n'
                        f'output="{output}"\ncalibration_file="{calibration}"\n'
                        'variance=100\nsleep_seconds=0\n[metrics]\n'
                        f'select=[{selection}"external.demo.Sequence"]\n'
                        f'[[adapters]]\nname="demo"\npath="{path.name}"\n'
                    )
                    result = self.run_configured(config, env={"GREEN_TEST_FAIL_AFTER": "0"})
                    self.assertIn("Skipping unavailable external metric", result.stderr)
                    if other_metric:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        with output.open(newline="", encoding="utf-8") as file:
                            rows = list(csv.reader(file, skipinitialspace=True))
                        self.assertEqual(rows[0][-1], "Cores")
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("No selected metrics are available", result.stderr)
                        self.assertFalse(output.exists())

    def test_overtime_workload_failure_does_not_dump_csv(self):
        path = self.build_adapter("configured_metric_adapter.c")
        result = self.run_configured(
            self.overtime_config(path, '["external.demo.Negative"]'),
            "-command", "exit 12",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.directory / "out.csv").exists())

    def test_overtime_system_metrics_are_selected_without_adapter(self):
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        result = self.run_configured(
            '[measurement]\nmode="over-time"\ncommand="sleep 0.05"\n'
            f'calibration_file="{calibration}"\noutput="{self.directory / "out.csv"}"\n'
            'interval_us=10000\n[metrics]\n'
            'select=["system.memory", "system.cores", "system.workload_processes"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with (self.directory / "out.csv").open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0][4:], ["CPU_Memory_KB", "Cores", "Workload_Processes"])
        self.assertGreaterEqual(len(rows), 3)
        self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
        self.assertTrue(all(float(row[5]) > 0 for row in rows[1:]))

    def test_calibration_path_does_not_execute_shell_commands(self):
        path = self.build_adapter("configured_metric_adapter.c")
        calibration = self.directory / "calib;touch marker;#"
        result = self.run_configured(
            self.overtime_config(path, '["external.demo.Negative"]'),
            "-calib-file", str(calibration), cwd=self.directory,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(calibration.exists())
        self.assertFalse((self.directory / "marker").exists())

    def test_invalid_cli_override_is_rejected(self):
        path = self.build_adapter("configured_metric_adapter.c")
        result = self.run_configured(
            self.overtime_config(path, '["external.demo.Negative"]'),
            "-interval-us", "0",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("interval_us", result.stderr)
        self.assertFalse((self.directory / "out.csv").exists())

    def test_overtime_rejects_missing_metrics(self):
        path = self.build_adapter("configured_metric_adapter.c")
        for selected, message in (
            ('["external.demo.Missing"]', "has no metric"),
            ('["cpu.Package"]', "not provided by the selected backend"),
        ):
            with self.subTest(selected=selected):
                result = self.run_configured(self.overtime_config(path, selected))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertIn("library's .c and .h files", result.stderr)
                self.assertIn("external.<adapter>.<metric>", result.stderr)
                if selected == '["cpu.Package"]':
                    self.assertIn("No GPU energy backend active", result.stderr)
                self.assertFalse((self.directory / "out.csv").exists())

    def test_unavailable_adapter_does_not_prevent_other_metrics(self):
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        for source, metric in (("configured_metric_adapter.c", "Missing"),
                               ("invalid_metric_adapter.c", "Negative")):
            path = self.build_adapter(source, output=f"{source}.so")
            for mode in ("standard", "over-time"):
                with self.subTest(source=source, mode=mode):
                    output = self.directory / f"{source}-{mode}.csv"
                    result = self.run_configured(
                        f'[measurement]\nmode="{mode}"\ncommand="sleep 0.02"\n'
                        f'output="{output}"\ncalibration_file="{calibration}"\n'
                        'variance=100\nsleep_seconds=0\n[metrics]\n'
                        f'select=["system.cores", "external.demo.{metric}"]\n'
                        f'[[adapters]]\nname="demo"\npath="{path.name}"\n'
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f"Skipping unavailable external metric 'external.demo.{metric}'",
                                  result.stderr)
                    with output.open(newline="", encoding="utf-8") as file:
                        rows = list(csv.reader(file, skipinitialspace=True))
                    self.assertEqual(rows[0][-1], "Cores")

    def test_selected_backend_failures_abort_without_csv(self):
        source = ROOT / "src" / "tests" / "fake_backend.c"
        backend_dir = self.directory / "backends" / "cpu" / "fake"
        backend_dir.mkdir(parents=True)
        config = (
            '[measurement]\nmode="over-time"\ncommand="sleep 0.1"\n'
            f'output="{self.directory / "out.csv"}"\nsleep_seconds=0\n'
            '[metrics]\nselect=["cpu.Package"]\n'
        )
        for flag, error in (("FAKE_INIT_FAILURE", "Failed to start selected CPU backend"),
                            ("FAKE_METRIC_UNAVAILABLE", "Skipping unavailable backend metric 'cpu.Package'")):
            with self.subTest(failure=flag):
                subprocess.run(
                    ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
                     "-DFAKE_DETECT=1", f"-D{flag}", str(source),
                     "-o", str(backend_dir / "backend.so")],
                    check=True, capture_output=True, text=True,
                )
                result = self.run_configured(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertFalse((self.directory / "out.csv").exists())

    def test_overtime_rejects_same_adapter_under_two_aliases(self):
        path = self.build_adapter("configured_metric_adapter.c")
        text = self.overtime_config(
            path, '["external.demo.Negative", "external.other.Sequence"]'
        ) + f'[[adapters]]\nname="other"\npath="{path.name}"\n'
        result = self.run_configured(text)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already used", result.stderr)
        self.assertFalse((self.directory / "out.csv").exists())

    def test_standard_backend_failures_preserve_output(self):
        source = ROOT / "src" / "tests" / "fake_backend.c"
        backend_dir = self.directory / "backends" / "cpu" / "fake"
        backend_dir.mkdir(parents=True)
        output = self.directory / "out.csv"
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        config = (
            '[measurement]\nmode="standard"\ncommand="sleep 0.1"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
            '[metrics]\nselect=["cpu.Package"]\n'
        )
        for flag, error in (("FAKE_INIT_FAILURE", "Failed to initialize or probe"),
                            ("FAKE_METRIC_UNAVAILABLE", "Skipping unavailable backend metric 'cpu.Package'"),
                            ("FAKE_METRIC_NAN", "Skipping unavailable backend metric 'cpu.Package'")):
            with self.subTest(failure=flag):
                subprocess.run(
                    ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
                     "-DFAKE_DETECT=1", f"-D{flag}", str(source),
                     "-o", str(backend_dir / "backend.so")],
                    check=True, capture_output=True, text=True,
                )
                output.write_text("preserved\n", encoding="utf-8")
                result = self.run_configured(config)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertEqual(output.read_text(encoding="utf-8"), "preserved\n")
                self.assertEqual(list(self.directory.glob("out.csv.tmp.*")), [])

    def test_backend_preflight_skips_failed_metrics_and_keeps_other_metrics(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        for flag in ("FAKE_INIT_FAILURE", "FAKE_METRIC_UNAVAILABLE"):
            subprocess.run(
                ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
                 "-DFAKE_DETECT=1", f"-D{flag}", str(ROOT / "src" / "tests" / "fake_backend.c"),
                 "-o", str(plugin)], check=True, capture_output=True, text=True,
            )
            for mode in ("standard", "over-time"):
                with self.subTest(failure=flag, mode=mode):
                    output = self.directory / f"{flag}-{mode}.csv"
                    result = self.run_configured(
                        f'[measurement]\nmode="{mode}"\ncommand="sleep 0.06"\n'
                        f'output="{output}"\ncalibration_file="{calibration}"\n'
                        'variance=100\nsleep_seconds=0\n[metrics]\n'
                        'select=["system.cores", "cpu.Package", "cpu.BoardPower"]\n'
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("Skipping unavailable backend metric 'cpu.Package'", result.stderr)
                    with output.open(newline="", encoding="utf-8") as file:
                        rows = list(csv.reader(file, skipinitialspace=True))
                    self.assertNotIn("cpu.Package_J", rows[0])
                    self.assertIn("Cores", rows[0])
                    if flag == "FAKE_METRIC_UNAVAILABLE":
                        self.assertEqual(rows[0][-1], "cpu.BoardPower_W")
                        self.assertTrue(all(float(row[-1]) == 8.5 for row in rows[1:]))
                    else:
                        self.assertNotIn("cpu.BoardPower_W", rows[0])

    def test_standard_backend_uses_multiple_windows(self):
        source = ROOT / "src" / "tests" / "fake_backend.c"
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", str(source), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "out.csv"
        result = self.run_configured(
            '[measurement]\nmode="standard"\ncommand="sleep 0.23"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
            '[metrics]\nselect=["cpu.Package"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms", "cpu.Package_J"])
        self.assertEqual(len(rows), 2)
        self.assertGreaterEqual(float(rows[1][4]), 3)

    def test_standard_process_scoped_metric(self):
        source = ROOT / "src" / "tests" / "fake_backend.c"
        plugin = self.directory / "backends" / "gpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="GPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_GPU", str(source), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        marker = self.directory / "started"
        output = self.directory / "out.csv"
        result = self.run_configured(
            '[measurement]\nmode="standard"\n'
            f'command="touch {marker}; sleep 0.1"\noutput="{output}"\n'
            f'calibration_file="{self.directory / "calibration.txt"}"\n'
            'sleep_seconds=0\nvariance=100\n'
            '[metrics]\nselect=["gpu.VRAM"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(marker.exists())
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms", "gpu.VRAM_MiB"])
        self.assertGreaterEqual(float(rows[1][4]), 8)

    def test_standard_process_metric_unavailable_is_marked(self):
        plugin = self.directory / "backends" / "gpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="GPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_GPU", "-DFAKE_PROCESS_UNAVAILABLE",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "out.csv"
        output.write_text("preserved\n", encoding="utf-8")
        result = self.run_configured(
            '[measurement]\nmode="standard"\ncommand="sleep 0.06"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
            '[metrics]\nselect=["gpu.VRAM"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gpu.VRAM' was unavailable during the workload", result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[1][-1], "NOT_SUPPORTED")

    def test_standard_configured_matches_cli_metric_semantics(self):
        for category in ("cpu", "gpu"):
            plugin = self.directory / "backends" / category / "fake" / "backend.so"
            plugin.parent.mkdir(parents=True)
            subprocess.run(
                ["cc", "-fPIC", "-shared", '-DFAKE_NAME="Fake"', "-DFAKE_PRIORITY=0",
                 "-DFAKE_DETECT=1", *(["-DFAKE_GPU"] if category == "gpu" else []),
                 str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
                check=True, capture_output=True, text=True,
            )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        marker = self.directory / "runs"
        command = f"echo run >> {marker}; sleep 0.12"
        config_output = self.directory / "configured.csv"
        plain_output = self.directory / "plain.csv"
        result = self.run_configured(
            '[measurement]\nmode="standard"\n'
            f'command="{command}"\noutput="{config_output}"\n'
            f'calibration_file="{calibration}"\nvariance=100\nrepetitions=2\n'
            '[metrics]\nselect=["system.memory", "system.cores", "system.workload_processes", '
            '"system.running_processes", "cpu.BoardPower", "gpu.VRAM"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        environment = os.environ.copy()
        environment["GREENMEASURER_BACKEND_DIR"] = str(self.directory / "backends")
        result = subprocess.run(
            [str(ROOT / "src" / "tests" / "legacy-engine"), "--standard", "-command", command,
             "-output", str(plain_output), "-calib-file", str(calibration),
             "-variance", "100", "-n_times", "2"],
            env=environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8").splitlines(), ["run"] * 4)
        for output in (config_output, plain_output):
            with output.open(newline="", encoding="utf-8") as file:
                reader = csv.DictReader(file, skipinitialspace=True)
                rows = list(reader)
            self.assertEqual(len(rows), 2)
            for row in rows:
                self.assertGreater(float(row["CPU_Memory_KB"]), 0)
                self.assertGreater(float(row["Cores"]), 0)
                self.assertGreater(float(row["Workload_Processes_Peak"]), 0)
                self.assertGreaterEqual(float(row["System_Running_Processes_Peak"]), 0)
                self.assertEqual(float(row["cpu.BoardPower_W"]), 8.5)
                self.assertGreaterEqual(float(row["gpu.VRAM_MiB"]), 8)

    def test_overtime_configured_matches_cli_metric_semantics(self):
        for category in ("cpu", "gpu"):
            plugin = self.directory / "backends" / category / "fake" / "backend.so"
            plugin.parent.mkdir(parents=True)
            subprocess.run(
                ["cc", "-fPIC", "-shared", '-DFAKE_NAME="Fake"', "-DFAKE_PRIORITY=0",
                 "-DFAKE_DETECT=1", *(["-DFAKE_GPU"] if category == "gpu" else []),
                 str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
                check=True, capture_output=True, text=True,
            )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        config_output = self.directory / "configured.csv"
        plain_output = self.directory / "plain.csv"
        result = self.run_configured(
            '[measurement]\nmode="over-time"\ncommand="sleep 0.15"\n'
            f'output="{config_output}"\ncalibration_file="{calibration}"\n'
            'variance=100\ninterval_us=20000\n'
            '[metrics]\nselect=["system.cores", "cpu.Package", '
            '"cpu.BoardPower", "gpu.GPU", "gpu.VRAM"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        environment = os.environ.copy()
        environment["GREENMEASURER_BACKEND_DIR"] = str(self.directory / "backends")
        result = subprocess.run(
            [str(ROOT / "src" / "tests" / "legacy-engine"), "--over-time", "-command", "sleep 0.15",
             "-output", str(plain_output), "-calib-file", str(calibration),
             "-variance", "100", "-interval-us", "20000"],
            env=environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for output in (config_output, plain_output):
            with output.open(newline="", encoding="utf-8") as file:
                rows = list(csv.DictReader(file, skipinitialspace=True))
            self.assertGreaterEqual(len(rows), 3)
            for row in rows:
                self.assertGreater(float(row["Cores"]), 0)
                self.assertEqual(float(row["cpu.Package_J"]), 1)
                self.assertEqual(float(row["cpu.BoardPower_W"]), 8.5)
                self.assertEqual(float(row["gpu.GPU_J"]), 1)
                self.assertGreaterEqual(float(row["gpu.VRAM_MiB"]), 8)

    def test_standard_frequency_selection(self):
        cores = os.sysconf("SC_NPROCESSORS_ONLN")
        if not cores or cores > 256:
            self.skipTest("core frequency sampler is limited to 256 cores")
        for core in range(cores):
            path = Path(f"/sys/devices/system/cpu/cpu{core}/cpufreq/scaling_cur_freq")
            if not os.access(path, os.R_OK):
                self.skipTest("core frequency is not readable on this host")
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "out.csv"
        result = self.run_configured(
            '[measurement]\nmode="standard"\ncommand="sleep 0.12"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
            '[metrics]\nselect=["system.core_frequency"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0][4:], [f"Core{i}_Avg_Freq_MHz" for i in range(cores)])
        self.assertEqual(len(rows), 2)
        self.assertEqual(len(rows[1]), len(rows[0]))
        self.assertTrue(all(float(value) > 0 for value in rows[1][4:]))

    def test_backend_sampling_failures_are_marked_when_configured(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_SAMPLE_FAILURE",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        environment = os.environ.copy()
        environment["GREENMEASURER_BACKEND_DIR"] = str(self.directory / "backends")
        for mode in ("standard", "over-time"):
            for configured in (False, True):
                with self.subTest(mode=mode, configured=configured):
                    output = self.directory / f"{mode}-{configured}.csv"
                    output.write_text("preserved\n", encoding="utf-8")
                    if configured:
                        result = self.run_configured(
                            f'[measurement]\nmode="{mode}"\ncommand="sleep 0.1"\n'
                            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
                            '[metrics]\nselect=["cpu.Package"]\n'
                        )
                    else:
                        result = subprocess.run(
                            [str(ROOT / "src" / "tests" / "legacy-engine"), f"--{mode}", "-command", "sleep 0.1",
                             "-output", str(output), "-calib-file", str(calibration),
                             "-variance", "100"],
                            env=environment, text=True, capture_output=True, timeout=15,
                        )
                    if configured:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn("backend sample failed", result.stderr)
                        with output.open(newline="", encoding="utf-8") as file:
                            rows = list(csv.reader(file, skipinitialspace=True))
                        self.assertTrue(all(row[-1] == "NOT_SUPPORTED" for row in rows[1:]))
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("[ERROR]", result.stderr)
                        self.assertEqual(output.read_text(encoding="utf-8"), "preserved\n")

    def test_partial_backend_energy_is_not_reported_as_complete(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_SAMPLE_FAIL_AFTER_ONE",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        for mode in ("standard", "over-time"):
            with self.subTest(mode=mode):
                output = self.directory / f"partial-{mode}.csv"
                result = self.run_configured(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.16"\n'
                    f'output="{output}"\ncalibration_file="{calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\n'
                    'select=["system.cores", "cpu.Package"]\n'
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("backend sample failed", result.stderr)
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertTrue(all(float(row[-2]) > 0 for row in rows[1:]))
                self.assertEqual(rows[-1][-1], "NOT_SUPPORTED")
                if mode == "standard":
                    self.assertEqual(rows[1][-1], "NOT_SUPPORTED")
                else:
                    self.assertEqual(float(rows[1][-1]), 1.0)

    def test_failed_backend_metric_does_not_disable_other_metric(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_METRIC_FAIL_AFTER_TWO",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        for mode in ("standard", "over-time"):
            with self.subTest(mode=mode):
                output = self.directory / f"one-failure-{mode}.csv"
                result = self.run_configured(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.16"\n'
                    f'output="{output}"\ncalibration_file="{calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\n'
                    'select=["cpu.Package", "cpu.BoardPower"]\n'
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Failed to read selected metric 'cpu.Package'", result.stderr)
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows[-1][-2], "NOT_SUPPORTED")
                self.assertTrue(all(float(row[-1]) == 8.5 for row in rows[1:]))
                if mode == "standard":
                    self.assertEqual(rows[1][-2], "NOT_SUPPORTED")

    def test_standard_backend_peak_and_noncontiguous_windows(self):
        source = ROOT / "src" / "tests" / "fake_backend.c"
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_DEVICE_PEAK", "-DFAKE_NO_NEXT",
             str(source), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "out.csv"
        result = self.run_configured(
            '[measurement]\nmode="standard"\ncommand="sleep 0.23"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
            '[metrics]\nselect=["cpu.Package", "cpu.DevicePeak"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("energy between windows will not be recorded", result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms",
                                   "cpu.Package_J", "cpu.DevicePeak_units"])
        self.assertGreaterEqual(float(rows[1][4]), 3)
        self.assertEqual(float(rows[1][5]), 9)

    def test_standard_records_temperature_before_the_workload(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_TEMPERATURE_INCREASING",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "temperature.csv"
        result = self.run_configured(
            '[measurement]\nmode="standard"\ncommand="sleep 0.3"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\nvariance=100\n'
            '[metrics]\nselect=["cpu.Temperature"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        temperature_column = rows[0].index("cpu.Temperature_C")
        self.assertEqual(float(rows[1][temperature_column]), 42.0)

    def test_overtime_temperature_gate_runs_before_each_measurement(self):
        plugin = self.directory / "backends" / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_TEMPERATURE_INCREASING",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 42.0\n", encoding="utf-8")
        output = self.directory / "overtime-temperature.csv"
        result = self.run_configured(
            '[measurement]\nmode="over-time"\ncommand="sleep 0.08"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\n'
            'variance=0\nsleep_seconds=0\ninterval_us=10000\n'
            '[metrics]\nselect=["cpu.Package", "cpu.Temperature"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        temperature_column = rows[0].index("cpu.Temperature_C")
        temperatures = [float(row[temperature_column]) for row in rows[1:]]
        self.assertGreaterEqual(len(temperatures), 3)
        self.assertTrue(all(temperature >= 42.0 for temperature in temperatures))
        self.assertGreater(len(set(temperatures)), 1)

    def test_standard_repetitions_with_external_and_system_metrics(self):
        path = self.build_adapter("configured_metric_adapter.c")
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "standard.csv"
        runs = self.directory / "runs.txt"
        config = (
            '[measurement]\nmode="standard"\ncommand="sleep 0.02"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\n'
            'repetitions=3\ntimeout_hours=1\nvariance=100\n'
            '[metrics]\nselect=["system.cores", "external.demo.Sequence", '
            '"external.demo.Negative"]\n'
            f'[[adapters]]\nname="demo"\npath="{path.name}"\n'
        )
        result = self.run_configured(config, "-n_times", "2", "-time_out_limit", "1",
                                     "-command", f"echo run >> {runs}; sleep 0.02")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(runs.read_text(encoding="utf-8").splitlines(), ["run", "run"])
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms", "Cores",
                                   "external.demo.Sequence_count", "external.demo.Negative_units"])
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
        self.assertTrue(all(float(row[3]) >= 15 and int(row[4]) > 0 for row in rows[1:]))
        self.assertEqual([float(row[5]) for row in rows[1:]], [3.0, 5.0])
        self.assertEqual([float(row[6]) for row in rows[1:]], [-5.0, -5.0])

    def test_standard_failure_preserves_existing_output(self):
        path = self.build_adapter("configured_metric_adapter.c")
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "standard.csv"
        config = (
            '[measurement]\nmode="standard"\ncommand="true"\n'
            f'output="{output}"\ncalibration_file="{calibration}"\n'
            'repetitions=3\nvariance=100\n'
            '[metrics]\nselect=["external.demo.Sequence"]\n'
            f'[[adapters]]\nname="demo"\npath="{path.name}"\n'
        )
        for override, environment, error in (
            (("-command", "exit 7"), {}, "Standard workload failed"),
        ):
            with self.subTest(error=error):
                output.write_text("preserved\n", encoding="utf-8")
                result = self.run_configured(config, *override, env=environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertEqual(output.read_text(encoding="utf-8"), "preserved\n")
                self.assertEqual(list(self.directory.glob("standard.csv.tmp.*")), [])

        result = self.run_configured(config, env={"GREEN_TEST_FAIL_AFTER": "2"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Failed to read external metric", result.stderr)
        with output.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(len(rows), 4)
        self.assertIn("NOT_SUPPORTED", [row[-1] for row in rows[1:]])

    def test_standard_timeout_aborts_without_output(self):
        backends = self.directory / "backends"
        backends.mkdir()
        install_temperature_backend(backends)
        environment = os.environ.copy()
        environment["GREENMEASURER_BACKEND_DIR"] = str(backends)
        calibration = self.directory / "calibration.txt"
        calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        output = self.directory / "standard.csv"
        result = subprocess.run(
            [str(ROOT / "src" / "tests" / "legacy-engine"), "--configured-standard", "-command", "sleep 10",
             "-timeout-seconds", "1", "-calib-file", str(calibration), "-output", str(output),
             "--configured-system", "system.cores"],
            env=environment, text=True, capture_output=True, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exceeded its timeout", result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.directory.glob("standard.csv.tmp.*")), [])

    def test_selected_metrics_and_configurable_execution(self):
        result = self.validate(
            '[measurement]\nmode="over-time"\ncommand="sleep 1"\n'
            'interval_us=1000\npowercap=-1\n'
            '[metrics]\nselect=["cpu.Package", "cpu.DRAM", "system.running_processes"]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cannot yet be validated", result.stdout)

    def test_external_adapter_path_is_relative_to_config(self):
        (self.directory / "adapter.so").write_bytes(b"placeholder")
        result = self.validate(
            '[measurement]\nmode="standard"\nrepetitions=2\n'
            '[metrics]\nselect=["external.demo.Value"]\n'
            '[[adapters]]\nname="demo"\npath="adapter.so"\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_metric_and_undeclared_adapter_report_guidance(self):
        for selected, problem in (
            ("system.Missing", "no such system metric is registered"),
            ("external.missing.Value", "adapter 'missing' is not declared"),
        ):
            with self.subTest(metric=selected):
                result = self.validate(
                    '[measurement]\nmode="standard"\ncommand="true"\n'
                    f'[metrics]\nselect=["{selected}"]\n'
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(problem, result.stderr)
                self.assertIn("library's .c and .h files", result.stderr)
                self.assertIn("external.<adapter>.<metric>", result.stderr)

    def test_all_metrics(self):
        result = self.validate('[measurement]\nmode="standard"\n[metrics]\nselect="all"\n')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_adapter_check_reads_selected_metric(self):
        path = self.build_adapter("metric_adapter_read_failure.c")
        result = self.validate(
            self.adapter_config(path, '["external.demo.Available"]'), "--check-adapters"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Available (count): 42", result.stdout)
        self.assertNotIn("Unavailable", result.stderr)
        self.assertIn("CPU, GPU and system metric availability", result.stdout)

    def test_adapter_check_rejects_missing_metric_and_failed_read(self):
        path = self.build_adapter("metric_adapter_read_failure.c")
        for metric, message in (("Missing", "has no metric"), ("Unavailable", "Failed to read"),
                                ("NotFinite", "Failed to read")):
            with self.subTest(metric=metric):
                result = self.validate(
                    self.adapter_config(path, f'["external.demo.{metric}"]'),
                    "--check-adapters",
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertIn("library's .c and .h files", result.stderr)
                if metric != "Missing":
                    self.assertIn("exists in the adapter but could not be read", result.stderr)

    def test_example_adapter_is_checked(self):
        path = self.directory / "example.so"
        example = ROOT / "src" / "tests" / "fixtures" / "custom_metric"
        subprocess.run(
            ["cc", "-fPIC", "-shared", str(example / "adapter.c"),
             str(example / "processes.c"), "-o", str(path)],
            check=True, capture_output=True, text=True,
        )
        result = self.validate(
            self.adapter_config(path, '["external.demo.RunningProcesses"]'),
            "--check-adapters",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("RunningProcesses (count):", result.stdout)

    def test_adapter_check_all_skips_unavailable_metric(self):
        path = self.build_adapter("metric_adapter_read_failure.c")
        result = self.validate(self.adapter_config(path, '"all"'), "--check-adapters")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Available (count): 42", result.stdout)
        self.assertIn("Unavailable", result.stderr)
        self.assertIn("skipped", result.stderr)

    def test_adapter_metric_named_like_mode_is_selected_literally(self):
        path = self.build_adapter("metric_adapter_read_failure.c")
        result = self.validate(
            self.adapter_config(path, '["external.demo.--all"]'), "--check-adapters"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--all (count): 9", result.stdout)
        self.assertNotIn("Available", result.stdout)
        self.assertNotIn("Unavailable", result.stderr)

    def test_adapter_check_rejects_incompatible_abi(self):
        path = self.build_adapter("invalid_metric_adapter.c")
        result = self.validate(self.adapter_config(path, '"all"'), "--check-adapters")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incompatible metric adapter", result.stderr)

    def test_unselected_adapter_is_not_loaded(self):
        path = self.directory / "invalid.so"
        path.write_bytes(b"not a library")
        result = self.validate(
            self.adapter_config(path, '["system.cores"]'), "--check-adapters"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_bad_configurations(self):
        examples = [
            ('[measurement]\nmode="invalid"\n[metrics]\nselect="all"\n', "mode"),
            ('[measurement]\nmode="overtime"\n[metrics]\nselect="all"\n', "over-time"),
            ('[measurement]\nmode="over-time"\ninterval_us=true\n[metrics]\nselect="all"\n', "integer"),
            ('[measurement]\nmode="standard"\ninterval_us=1\n[metrics]\nselect="all"\n', "only valid"),
            ('[measurement]\nmode="over-time"\npowercap=0\n[metrics]\nselect="all"\n', "powercap"),
            ('[measurement]\nmode="standard"\n[metrics]\nselect=["cpu.Foo", "cpu.Foo"]\n', "duplicate"),
            ('[measurement]\nmode="standard"\n[metrics]\nselect=["external.x.Foo"]\n', "unknown"),
            ('[measurement]\nmode="standard"\nunknown=1\n[metrics]\nselect="all"\n', "unknown keys"),
            ('[measurement]\nmode="standard"\n[metrics]\nselect="all"\n'
             '[[adapters]]\nname="a"\npath="missing.so"\n', "existing file"),
            ('[measurement]\nmode="standard"\n[metrics]\nselect=[\n', "Error:"),
        ]
        for contents, expected in examples:
            with self.subTest(expected=expected, contents=contents):
                result = self.validate(contents)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
