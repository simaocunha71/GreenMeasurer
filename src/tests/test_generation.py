import csv
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from temperature_fixture import install_temperature_backend


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts" / "generate_config.py"


class GenerationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.config = self.directory / "config.toml"
        self.build = self.directory / "generated files"
        self.csv = self.directory / "results.csv"
        self.calibration = self.directory / "calibration.txt"
        self.calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        self.backends = self.directory / "backends"
        self.backends.mkdir()
        install_temperature_backend(self.backends)
        self.environment = os.environ.copy()
        self.environment["GREENMEASURER_BACKEND_DIR"] = str(self.backends)

    def tearDown(self):
        self.temporary.cleanup()

    def generate(self):
        return subprocess.run(
            [sys.executable, str(GENERATOR), str(self.config), "--directory", str(self.build)],
            text=True, capture_output=True, env=self.environment, timeout=60,
        )

    def test_public_launcher_uses_root_configuration_by_default(self):
        subprocess.run(["make", "-C", str(ROOT / "src")], check=True,
                       capture_output=True, text=True)
        result = subprocess.run(
            [str(ROOT / "src" / "main"), "--directory", str(self.build)],
            cwd=self.directory, text=True, capture_output=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        source = (self.build / "greenmeasurer.c").read_text(encoding="utf-8")
        self.assertTrue((self.build / "greenmeasurer").exists())
        explicit = self.directory / "explicit"
        result = subprocess.run(
            [str(ROOT / "src" / "main"), "--config", str(ROOT / "configuration.toml"),
             "--directory", str(explicit)],
            cwd=self.directory, text=True, capture_output=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(source, (explicit / "greenmeasurer.c").read_text(encoding="utf-8"))

    def test_select_all_discovers_system_backends_and_external_metrics_in_both_modes(self):
        adapter = self.directory / "adapter.so"
        subprocess.run(
            ["cc", "-fPIC", "-shared", str(ROOT / "src" / "tests" / "configured_metric_adapter.c"),
             "-o", str(adapter)], check=True, capture_output=True, text=True,
        )
        (self.directory / "count.h").write_text("int get_count(void);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(void) { return 12; }\n', encoding="utf-8",
        )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"all-{mode}"
                self.config.write_text(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.1"\n'
                    f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\nselect="all"\n'
                    '[[adapters]]\nname="demo"\npath="adapter.so"\n'
                    '[[external_metrics]]\nname="source"\nmetric="Count"\nunit="count"\n'
                    'function="get_count"\nheader="count.h"\nsources=["count.c"]\n'
                    'return_type="int"\n', encoding="utf-8",
                )
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(self.csv.exists())
                self.assertTrue((self.build / "source_adapter.so").exists())
                for category in ("cpu", "gpu"):
                    plugin = self.backends / category / "toy" / "backend.so"
                    plugin.parent.mkdir(parents=True, exist_ok=True)
                    source = (ROOT / "src" / "tests" / "fixtures" / "toy_backends" / category /
                              f"toy_{category}" / f"toy_{category}.c")
                    subprocess.run(
                        ["cc", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
                         str(source), "-o", str(plugin)], check=True, capture_output=True,
                    )
                result = subprocess.run([str(self.build / "greenmeasurer")], text=True,
                                        capture_output=True, env=self.environment, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                with self.csv.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                columns = rows[0]
                expected = {
                    "CPU_Memory_KB", "Cores",
                    "Workload_Processes_Peak" if mode == "standard" else "Workload_Processes",
                    "System_Running_Processes_Peak" if mode == "standard" else "System_Running_Processes",
                    "cpu.Energy_J", "cpu.LoadAvg_1m", "gpu.Energy_J", "gpu.Power_W",
                    "gpu.Memory_MiB", "external.demo.Negative_units",
                    "external.demo.Sequence_count", "external.source.Count_count",
                }
                self.assertTrue(expected.issubset(columns), (columns, result.stderr))
                freq_columns = [column for column in columns if column.startswith("Core0_")]
                if "system.core_frequency" not in result.stderr:
                    self.assertEqual(len(freq_columns), 1)
                    self.assertEqual(len([column for column in columns
                                          if column.startswith("Core") and "Freq_MHz" in column]),
                                     int(float(rows[1][columns.index("Cores")])))
                else:
                    self.assertFalse(freq_columns)
                self.assertEqual(len(columns), len(set(columns)))
                self.assertTrue(all(len(row) == len(columns) for row in rows))
                self.assertTrue(all(float(row[columns.index("external.source.Count_count")]) == 12
                                    for row in rows[1:]))
                self.assertTrue(all(float(row[columns.index("gpu.Power_W")]) == 5
                                    for row in rows[1:]))
                self.csv.unlink()

    def test_new_metric_examples_emit_changing_system_uptime(self):
        config_directory = ROOT / "configurations" / "new-metrics"
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"new-metric-{mode}"
                config = config_directory / f"{mode}.toml"
                result = subprocess.run(
                    [sys.executable, str(GENERATOR), str(config), "--directory", str(self.build)],
                    text=True, capture_output=True, env=self.environment, timeout=60,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run(
                    [str(self.build / "greenmeasurer")], text=True, capture_output=True,
                    env=self.environment, cwd=self.directory, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                output = self.directory / f"greenmeasurer-new-metric-{mode}.csv"
                with output.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                column = rows[0].index("external.system.UptimeSeconds_s")
                uptime_values = [float(row[column]) for row in rows[1:]]
                self.assertGreater(len(set(uptime_values)), 1)

    def test_select_all_keeps_failed_runtime_metrics_and_skips_missing_adapter(self):
        adapter = self.directory / "adapter.so"
        missing = self.directory / "missing.so"
        for path in (adapter, missing):
            subprocess.run(
                ["cc", "-fPIC", "-shared",
                 str(ROOT / "src" / "tests" / "configured_metric_adapter.c"),
                 "-o", str(path)], check=True, capture_output=True, text=True,
            )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                if not missing.exists():
                    missing.touch()
                self.build = self.directory / f"all-runtime-{mode}"
                self.config.write_text(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.05"\n'
                    f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\nselect="all"\n'
                    '[[adapters]]\nname="demo"\npath="adapter.so"\n'
                    '[[adapters]]\nname="missing"\npath="missing.so"\n', encoding="utf-8",
                )
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                missing.unlink()
                env = self.environment.copy()
                env["GREEN_TEST_FAIL_AFTER"] = "2"
                result = subprocess.run([str(self.build / "greenmeasurer")], text=True,
                                        capture_output=True, env=env, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Skipping unavailable adapter 'missing'", result.stderr)
                with self.csv.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                for name in ("external.demo.Negative_units", "external.demo.Sequence_count"):
                    self.assertIn(name, rows[0])
                    self.assertTrue(all(row[rows[0].index(name)] == "NOT_SUPPORTED"
                                        for row in rows[1:]))
                self.assertFalse(any(name.startswith("external.missing.") for name in rows[0]))
                self.csv.unlink()

    def test_select_all_skips_unavailable_backend_metric(self):
        plugin = self.backends / "cpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="CPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_METRIC_UNAVAILABLE",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"all-backend-failure-{mode}"
                self.config.write_text(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.05"\n'
                    f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\nselect="all"\n',
                    encoding="utf-8",
                )
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(self.build / "greenmeasurer")], text=True,
                                        capture_output=True, env=self.environment, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Skipping unavailable backend metric 'cpu.Package'", result.stderr)
                with self.csv.open(newline="", encoding="utf-8") as file:
                    header = next(csv.reader(file, skipinitialspace=True))
                self.assertNotIn("cpu.Package_J", header)
                self.assertIn("cpu.Temperature_C", header)
                self.assertIn("cpu.BoardPower_W", header)
                self.csv.unlink()

    def test_select_all_skips_unavailable_metrics_and_broken_source(self):
        adapter = self.directory / "adapter.so"
        subprocess.run(
            ["cc", "-fPIC", "-shared", str(ROOT / "src" / "tests" / "metric_adapter_read_failure.c"),
             "-o", str(adapter)], check=True, capture_output=True, text=True,
        )
        (self.directory / "broken.h").write_text("int actual(void);\n", encoding="utf-8")
        (self.directory / "broken.c").write_text("int actual(void) { return 1; }\n", encoding="utf-8")
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"all-failures-{mode}"
                self.config.write_text(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.05"\n'
                    f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
                    'variance=100\nsleep_seconds=0\n[metrics]\nselect="all"\n'
                    '[[adapters]]\nname="demo"\npath="adapter.so"\n'
                    '[[external_metrics]]\nname="broken"\nmetric="Broken"\nunit="count"\n'
                    'function="missing"\nheader="broken.h"\nsources=["broken.c"]\n'
                    'return_type="int"\n', encoding="utf-8",
                )
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("skipping", result.stderr)
                result = subprocess.run([str(self.build / "greenmeasurer")], text=True,
                                        capture_output=True, env=self.environment, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Skipping unavailable external metric", result.stderr)
                with self.csv.open(newline="", encoding="utf-8") as file:
                    header = next(csv.reader(file, skipinitialspace=True))
                self.assertIn("Cores", header)
                self.assertIn("external.demo.Available_count", header)
                self.assertIn("external.demo.--all_count", header)
                self.assertNotIn("external.demo.Unavailable_count", header)
                self.assertNotIn("external.demo.NotFinite_count", header)
                self.assertFalse(any("external.broken" in name for name in header))
                self.assertEqual([name for name in header if name.startswith(("cpu.", "gpu."))],
                                 ["cpu.Temperature_C"])
                self.csv.unlink()

    def test_public_launcher_rejects_legacy_modes_without_running_workload(self):
        subprocess.run(["make", "-C", str(ROOT / "src"), "main"], check=True,
                       capture_output=True, text=True)
        marker = self.directory / "workload-ran"
        for mode in ("--standard", "--overtime", "--configured-standard",
                     "--configured-overtime", "--temperature-calibration",
                     "--powercap-calibration", "--csv-header", "--overtime-csv-header"):
            with self.subTest(mode=mode):
                result = subprocess.run(
                    [str(ROOT / "src" / "main"), mode, "-command", f"touch '{marker}'",
                     "-output", str(self.csv)], text=True, capture_output=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("TOML configuration", result.stderr)
                self.assertFalse(marker.exists())
                self.assertFalse(self.csv.exists())

    def test_public_launcher_generates_without_running(self):
        subprocess.run(["make", "-C", str(ROOT / "src"), "main"], check=True,
                       capture_output=True, text=True)
        marker = self.directory / "workload-ran"
        self.config.write_text(
            f'[measurement]\nmode="standard"\ncommand="touch {marker}"\n'
            f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
            'variance=100\nsleep_seconds=0\n[metrics]\nselect=["system.cores"]\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(ROOT / "src" / "main"), "--config", str(self.config),
             "--directory", str(self.build)], text=True, capture_output=True,
            env=self.environment, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(self.csv.exists())
        result = subprocess.run([str(self.build / "greenmeasurer")], text=True,
                                capture_output=True, env=self.environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(marker.exists())
        self.assertTrue(self.csv.exists())

    def test_calibration_configurations_are_generated_and_validated(self):
        plugin = self.backends / "cpu" / "toy" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
             str(ROOT / "src" / "tests" / "fixtures" / "toy_backends" / "cpu" / "toy_cpu" / "toy_cpu.c"),
             "-o", str(plugin)], check=True, capture_output=True, text=True,
        )
        target = self.directory / "new-calibration"
        self.config.write_text(
            '[measurement]\nmode="temperature-calibration"\nsleep_seconds=0\n'
            f'calibration_file="{target}"\n', encoding="utf-8",
        )
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.build.exists())
        self.assertIn("Average temperature:", target.read_text(encoding="utf-8"))
        result = subprocess.run(
            ["bash", str(ROOT / "scripts" / "run_powercap_calibration.sh"),
             str(self.config), str(self.directory / "wrong-mode")],
            text=True, capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("measurement.mode must be 'powercap-calibration'", result.stderr)
        self.assertFalse((self.directory / "wrong-mode").exists())

        self.config.write_text(
            '[measurement]\nmode="temperature-calibration"\nsleep_seconds=0\n'
            f'calibration_file="{self.directory / "missing" / "calibration"}"\n',
            encoding="utf-8",
        )
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to open calibration file", result.stderr)

        self.build = self.directory / "powercap"
        self.config.write_text(
            '[measurement]\nmode="powercap-calibration"\ncommand="true"\n'
            'powercap_min=5\npowercap_max=4\n', encoding="utf-8",
        )
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("powercap_min", result.stderr)
        self.assertFalse(self.build.exists())
        self.config.write_text(self.config.read_text().replace("powercap_max=4", "powercap_max=6"))
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.build / "greenmeasurer").exists())

    def test_batch_generation_then_execution(self):
        batch = self.directory / "batch"
        configs = []
        for mode in ("standard", "overtime"):
            config = self.directory / f"{mode}.toml"
            output = self.directory / f"{mode}.csv"
            config.write_text(
                f'[measurement]\nmode="{mode}"\ncommand="sleep 0.02"\n'
                f'output="{output}"\ncalibration_file="{self.calibration}"\n'
                'sleep_seconds=0\nvariance=100\n[metrics]\nselect=["system.cores"]\n',
                encoding="utf-8",
            )
            configs.append(config)
        script = ROOT / "scripts" / "measure.sh"
        result = subprocess.run(["bash", str(script), "generate", str(batch),
                                 *map(str, configs)], env=self.environment, text=True,
                                capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.directory / "standard.csv").exists())
        self.assertFalse((self.directory / "overtime.csv").exists())
        result = subprocess.run(["bash", str(script), "run", str(batch)],
                                env=self.environment, text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.directory / "standard.csv").exists())
        self.assertTrue((self.directory / "overtime.csv").exists())

        invalid_batch = self.directory / "incomplete"
        invalid = self.directory / "invalid.toml"
        invalid.write_text('[measurement]\nmode="invalid"\n', encoding="utf-8")
        result = subprocess.run(["bash", str(script), "generate", str(invalid_batch),
                                 str(configs[0]), str(invalid)], env=self.environment,
                                text=True, capture_output=True, timeout=120)
        self.assertNotEqual(result.returncode, 0)
        result = subprocess.run(["bash", str(script), "run", str(invalid_batch)],
                                env=self.environment, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not completely generated", result.stderr)

        (batch / "run-0002" / "greenmeasurer").unlink()
        result = subprocess.run(["bash", str(script), "run", str(batch)],
                                env=self.environment, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No compiled executable", result.stderr)

    def external_config(self, function="get_count", arguments="[]", return_type="int",
                        selection='"system.cores", "external.count.Processes"', mode="overtime"):
        self.config.write_text(
            f'[measurement]\nmode="{mode}"\ncommand="sleep 0.06"\n'
            f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
            'sleep_seconds=0\nvariance=100\n'
            '[metrics]\n'
            f'select=[{selection}]\n'
            '[[external_metrics]]\nname="count"\nmetric="Processes"\nunit="count"\n'
            f'function="{function}"\nheader="count.h"\nsources=["count.c"]\n'
            f'return_type="{return_type}"\narguments={arguments}\nerror_value=-1\n',
            encoding="utf-8",
        )

    def test_external_function_compiles_and_runs_in_both_modes(self):
        (self.directory / "count.h").write_text("int get_count(int offset);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(int offset) { return 12 + offset; }\n',
            encoding="utf-8",
        )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"generated-{mode}"
                self.external_config(arguments='[{type="int", value=3}]', mode=mode)
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((self.build / "count_adapter.so").is_file())
                self.assertFalse(self.csv.exists())
                result = subprocess.run(
                    [str(self.build / "greenmeasurer")], cwd=self.directory,
                    env=self.environment, text=True, capture_output=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                with self.csv.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows[0][-1], "external.count.Processes_count")
                self.assertTrue(all(float(row[-1]) == 15 for row in rows[1:]))
                self.csv.unlink()

    def test_external_functions_have_separate_libraries_and_name_aliases(self):
        (self.directory / "count.h").write_text(
            "int first_count(void);\nint second_count(void);\n", encoding="utf-8",
        )
        (self.directory / "count.c").write_text(
            '#include "count.h"\n'
            'int first_count(void) { return 12; }\n'
            'int second_count(void) { return 34; }\n', encoding="utf-8",
        )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"separate-{mode}"
                self.config.write_text(
                    f'[measurement]\nmode="{mode}"\ncommand="sleep 0.06"\n'
                    f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
                    'sleep_seconds=0\nvariance=100\n[metrics]\n'
                    'select=["external.first.first_count", "external.second.Second"]\n'
                    '[[external_metrics]]\nname="first"\nmetric="First"\nunit="count"\n'
                    'function="first_count"\nheader="count.h"\nsources=["count.c"]\n'
                    'return_type="int"\n'
                    '[[external_metrics]]\nname="second"\nmetric="Second"\nunit="count"\n'
                    'function="second_count"\nheader="count.h"\nsources=["count.c"]\n'
                    'return_type="int"\n', encoding="utf-8",
                )
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((self.build / "first_adapter.so").is_file())
                self.assertTrue((self.build / "second_adapter.so").is_file())
                result = subprocess.run(
                    [str(self.build / "greenmeasurer")], cwd=self.directory,
                    env=self.environment, text=True, capture_output=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                with self.csv.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows[0][-2:], ["external.first.First_count",
                                                "external.second.Second_count"])
                self.assertTrue(all([float(row[-2]), float(row[-1])] == [12, 34]
                                    for row in rows[1:]))
                self.csv.unlink()

    def test_external_function_alias_cannot_select_the_same_metric_twice(self):
        (self.directory / "count.h").write_text("int get_count(void);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(void) { return 12; }\n', encoding="utf-8",
        )
        self.external_config(selection='"external.count.Processes", "external.count.get_count"')
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("selected more than once", result.stderr)
        self.assertFalse(self.build.exists())

    def test_missing_external_function_alias_skips_only_that_metric(self):
        (self.directory / "count.h").write_text("int get_count(void);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(void) { return 12; }\n', encoding="utf-8",
        )
        self.external_config(function="get_coun",
                             selection='"system.cores", "external.count.get_coun"')
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("get_count", result.stderr)
        result = subprocess.run(
            [str(self.build / "greenmeasurer")], cwd=self.directory, env=self.environment,
            text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0][-1], "Cores")

    def test_external_source_example_runs_from_generated_library(self):
        source = ROOT / "src" / "tests" / "fixtures" / "configurations" / "external-source.toml"
        example = ROOT / "src" / "tests" / "fixtures" / "custom_metric"
        text = source.read_text(encoding="utf-8")
        text = text.replace("../custom_metric/", f"{example}/")
        text = text.replace('command = "sleep 1"', 'command = "sleep 0.06"')
        text = text.replace('output = "external-source.csv"',
                            f'output = "{self.csv}"\ncalibration_file = "{self.calibration}"')
        self.config.write_text(text, encoding="utf-8")
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.build / "processes_adapter.so").is_file())
        result = subprocess.run(
            [str(self.build / "greenmeasurer")], cwd=self.directory, env=self.environment,
            text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0][-1], "external.processes.RunningProcesses_count")
        self.assertTrue(all(float(row[-1]) >= 0 for row in rows[1:]))

    def test_missing_generated_library_skips_metric_but_keeps_other_metrics(self):
        (self.directory / "count.h").write_text("int get_count(void);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(void) { return 12; }\n', encoding="utf-8",
        )
        for mode in ("standard", "overtime"):
            with self.subTest(mode=mode):
                self.build = self.directory / f"missing-library-{mode}"
                self.external_config(mode=mode)
                result = self.generate()
                self.assertEqual(result.returncode, 0, result.stderr)
                (self.build / "count_adapter.so").unlink()
                result = subprocess.run(
                    [str(self.build / "greenmeasurer")], cwd=self.directory,
                    env=self.environment, text=True, capture_output=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("external.count.Processes", result.stderr)
                self.assertIn("skipping", result.stderr.lower())
                with self.csv.open(newline="", encoding="utf-8") as file:
                    rows = list(csv.reader(file, skipinitialspace=True))
                self.assertEqual(rows[0][-1], "Cores")
                self.csv.unlink()

    def test_missing_external_function_is_skipped_with_suggestions(self):
        (self.directory / "count.h").write_text("int get_count(void);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(void) { return 12; }\n', encoding="utf-8",
        )
        self.external_config(function="get_coun")
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipping it", result.stderr)
        self.assertIn("get_count", result.stderr)
        result = subprocess.run(
            [str(self.build / "greenmeasurer")], cwd=self.directory,
            env=self.environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertNotIn("external.count.Processes_count", rows[0])

    def test_signature_mismatch_or_no_remaining_metrics_fails(self):
        (self.directory / "count.h").write_text("int get_count(int value);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\nint get_count(int value) { return value; }\n', encoding="utf-8",
        )
        self.external_config(selection='"external.count.Processes"')
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("signature does not match", result.stderr)
        self.assertIn("no selected metrics remain", result.stderr)

    def test_external_error_value_marks_runtime_sample(self):
        (self.directory / "count.h").write_text("int get_count(void);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\n'
            'int get_count(void) { static int reads; return reads++ ? -1 : 7; }\n',
            encoding="utf-8",
        )
        self.external_config()
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(
            [str(self.build / "greenmeasurer")], cwd=self.directory,
            env=self.environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("NOT_SUPPORTED", result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertTrue(all(row[-1] == "NOT_SUPPORTED" for row in rows[1:]))

    def test_external_double_argument_with_include_and_library(self):
        include = self.directory / "include"
        include.mkdir()
        (include / "count.h").write_text("double get_count(double value);\n", encoding="utf-8")
        (self.directory / "count.c").write_text(
            '#include "count.h"\n#include <math.h>\n'
            'double get_count(double value) { return sqrt(value); }\n', encoding="utf-8",
        )
        self.external_config(arguments='[{type="double", value=9.0}]', return_type="double")
        text = self.config.read_text(encoding="utf-8").replace(
            'header="count.h"', 'header="include/count.h"'
        )
        self.config.write_text(text + 'include_dirs=["include"]\nlibraries=["m"]\n', encoding="utf-8")
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(
            [str(self.build / "greenmeasurer")], cwd=self.directory,
            env=self.environment, text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertTrue(all(float(row[-1]) == 3.0 for row in rows[1:]))

    def test_generated_executable_embeds_selection_and_runs_separately(self):
        example = ROOT / "src" / "tests" / "fixtures" / "custom_metric"
        adapter = self.directory / "adapter.so"
        subprocess.run(
            ["cc", "-fPIC", "-shared", str(example / "adapter.c"),
             str(example / "processes.c"), "-o", str(adapter)],
            check=True, capture_output=True, text=True,
        )
        self.config.write_text(
            '[measurement]\nmode="overtime"\ncommand="sleep 0.06"\n'
            f'language="test"\nprogram="fixture"\noutput="{self.csv}"\n'
            f'calibration_file="{self.calibration}"\ninterval_us=10000\n'
            '[metrics]\nselect=["system.cores", "external.demo.RunningProcesses"]\n'
            '[[adapters]]\nname="demo"\npath="adapter.so"\n',
            encoding="utf-8",
        )
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.csv.exists(), "generation must not execute the workload")
        source = self.build / "greenmeasurer.c"
        binary = self.build / "greenmeasurer"
        self.assertTrue(source.is_file())
        self.assertTrue(binary.is_file())
        self.assertIn("system.cores", source.read_text(encoding="utf-8"))
        self.assertIn("RunningProcesses", source.read_text(encoding="utf-8"))
        self.config.unlink()
        result = subprocess.run([str(binary)], cwd=self.directory, text=True,
                                capture_output=True, env=self.environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Timestamp_ms",
                                  "Cores", "external.demo.RunningProcesses_count"])
        self.assertGreaterEqual(len(rows), 3)
        self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
        self.assertTrue(all(float(row[4]) > 0 and float(row[5]) >= 0 for row in rows[1:]))
        result = subprocess.run([str(binary), "-output", "ignored.csv"], text=True,
                                capture_output=True, env=self.environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not accept overrides", result.stderr)

    def test_invalid_standard_fails_before_creating_directory(self):
        self.config.write_text(
            '[measurement]\nmode="standard"\n'
            '[metrics]\nselect=["system.memory"]\n', encoding="utf-8",
        )
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("measurement.command is required", result.stderr)
        self.assertFalse(self.build.exists())

    def test_generated_standard_runs_repetitions(self):
        adapter = self.directory / "adapter.so"
        subprocess.run(
            ["cc", "-fPIC", "-shared", str(ROOT / "src" / "tests" / "configured_metric_adapter.c"),
             "-o", str(adapter)], check=True, capture_output=True, text=True,
        )
        self.config.write_text(
            '[measurement]\nmode="standard"\ncommand="sleep 0.02"\n'
            f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
            'repetitions=2\nvariance=100\n[metrics]\n'
            'select=["system.cores", "external.demo.Sequence"]\n'
            '[[adapters]]\nname="demo"\npath="adapter.so"\n',
            encoding="utf-8",
        )
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.csv.exists())
        self.config.unlink()
        result = subprocess.run([str(self.build / "greenmeasurer")], cwd=self.directory,
                                text=True, capture_output=True, env=self.environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms", "Cores",
                                   "external.demo.Sequence_count"])
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(float(row[3]) >= 15 and int(row[4]) > 0 for row in rows[1:]))
        self.assertEqual([float(row[5]) for row in rows[1:]], [2, 3])

    def test_generated_standard_process_and_system_metrics(self):
        plugin = self.backends / "gpu" / "fake" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-fPIC", "-shared", '-DFAKE_NAME="GPU"', "-DFAKE_PRIORITY=0",
             "-DFAKE_DETECT=1", "-DFAKE_GPU",
             str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        self.config.write_text(
            '[measurement]\nmode="standard"\ncommand="sleep 0.1"\n'
            f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
            'variance=100\n[metrics]\n'
            'select=["system.memory", "system.workload_processes", "gpu.VRAM"]\n',
            encoding="utf-8",
        )
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.csv.exists())
        result = subprocess.run([str(self.build / "greenmeasurer")], cwd=self.directory,
                                text=True, capture_output=True, env=self.environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms",
                                  "CPU_Memory_KB", "Workload_Processes_Peak", "gpu.VRAM_MiB"])
        self.assertEqual(len(rows), 2)
        self.assertGreater(float(rows[1][4]), 0)
        self.assertGreater(float(rows[1][5]), 0)
        self.assertGreaterEqual(float(rows[1][6]), 8)

    def test_generated_cpu_gpu_overtime_values(self):
        for category in ("cpu", "gpu"):
            directory = self.backends / category / "toy"
            directory.mkdir(parents=True)
            source = ROOT / "src" / "tests" / "fixtures" / "toy_backends" / category / f"toy_{category}" / f"toy_{category}.c"
            subprocess.run(
                ["cc", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
                 str(source), "-o", str(directory / "backend.so")],
                check=True, capture_output=True, text=True,
            )
        self.config.write_text(
            '[measurement]\nmode="overtime"\ncommand="sleep 0.15"\n'
            f'language="test"\nprogram="fixture"\noutput="{self.csv}"\n'
            f'calibration_file="{self.calibration}"\ninterval_us=10000\nvariance=100\n'
            '[metrics]\nselect=["cpu.toy_cpu_metric_energy", '
            '"gpu.toy_gpu_metric_energy", "gpu.toy_gpu_metric_power"]\n',
            encoding="utf-8",
        )
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(self.build / "greenmeasurer")], cwd=self.directory,
                                text=True, capture_output=True, env=self.environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Timestamp_ms",
                                  "cpu.Energy_J", "gpu.Energy_J", "gpu.Power_W"])
        self.assertGreaterEqual(len(rows), 3)
        self.assertTrue(all(len(row) == len(rows[0]) for row in rows))
        elapsed = float(rows[-1][3]) / 1000
        cpu = sum(float(row[4]) for row in rows[1:])
        gpu = sum(float(row[5]) for row in rows[1:])
        self.assertAlmostEqual(cpu, 10 * elapsed, delta=0.1)
        self.assertAlmostEqual(gpu, 5 * elapsed, delta=0.05)
        self.assertTrue(all(float(row[6]) == 5 for row in rows[1:]))

    def test_generated_cpu_gpu_standard_values(self):
        for category in ("cpu", "gpu"):
            directory = self.backends / category / "toy"
            directory.mkdir(parents=True)
            source = ROOT / "src" / "tests" / "fixtures" / "toy_backends" / category / f"toy_{category}" / f"toy_{category}.c"
            subprocess.run(
                ["cc", "-fPIC", "-shared", "-I", str(ROOT / "src" / "include"),
                 str(source), "-o", str(directory / "backend.so")],
                check=True, capture_output=True, text=True,
            )
        self.config.write_text(
            '[measurement]\nmode="standard"\ncommand="sleep 0.17"\n'
            f'output="{self.csv}"\ncalibration_file="{self.calibration}"\n'
            'repetitions=2\nvariance=100\n[metrics]\n'
            'select=["cpu.toy_cpu_metric_energy", "gpu.toy_gpu_metric_energy", '
            '"gpu.toy_gpu_metric_power"]\n',
            encoding="utf-8",
        )
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.csv.exists())
        result = subprocess.run([str(self.build / "greenmeasurer")], cwd=self.directory,
                                text=True, capture_output=True, env=self.environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.csv.open(newline="", encoding="utf-8") as file:
            rows = list(csv.reader(file, skipinitialspace=True))
        self.assertEqual(rows[0], ["Language", "Program", "Powercap", "Time_ms",
                                  "cpu.Energy_J", "gpu.Energy_J", "gpu.Power_W"])
        self.assertEqual(len(rows), 3)
        for row in rows[1:]:
            elapsed = float(row[3]) / 1000
            self.assertAlmostEqual(float(row[4]), elapsed * 10, delta=0.1)
            self.assertAlmostEqual(float(row[5]), elapsed * 5, delta=0.05)
            self.assertEqual(float(row[6]), 5)

    def test_existing_directory_is_not_overwritten(self):
        self.config.write_text(
            '[measurement]\nmode="overtime"\ncommand="true"\n'
            '[metrics]\nselect=["system.cores"]\n', encoding="utf-8",
        )
        self.build.mkdir()
        target = self.build / "greenmeasurer.c"
        target.write_text("preserve", encoding="utf-8")
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(encoding="utf-8"), "preserve")


if __name__ == "__main__":
    unittest.main()
