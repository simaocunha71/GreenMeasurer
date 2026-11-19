import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEAVER = ROOT / "scripts" / "weave.py"
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "weaver"


class WeaverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        for name in ("toy.c", "toy.h", "toy.toml", "toy_hooks.toml"):
            shutil.copy(FIXTURES / name, cls.directory / name)
        subprocess.run(["gcc", "-c", "toy.c", "-o", "toy.o"], cwd=cls.directory, check=True)
        subprocess.run(["ar", "rcs", "libtoy.a", "toy.o"], cwd=cls.directory, check=True)
        cls.executable = cls.weave(cls.directory / "toy.toml", cls.directory / "build")
        cls.hooks = cls.weave(cls.directory / "toy_hooks.toml", cls.directory / "hooks")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @classmethod
    def weave(cls, config, out_dir, *extra):
        result = subprocess.run(
            [sys.executable, str(WEAVER), str(config), "-o", str(out_dir), *extra],
            text=True, capture_output=True, timeout=120)
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        return out_dir / "greenmeasurer"

    def run_engine(self, *arguments, executable=None, **environment):
        output = self.directory / "result.csv"
        output.unlink(missing_ok=True)
        log = self.directory / "toy.log"
        log.unlink(missing_ok=True)
        env = {**os.environ, "TOY_LOG": str(log), **environment}
        result = subprocess.run(
            [str(executable or self.executable), "-output", str(output), *arguments],
            text=True, capture_output=True, env=env, timeout=60)
        rows = []
        if output.exists():
            with output.open() as source:
                rows = list(csv.DictReader(source))
        events = log.read_text().split() if log.exists() else []
        return result, rows, events

    def test_default_output_directory_uses_generated_prefix(self):
        result = subprocess.run(
            [sys.executable, str(WEAVER), str(self.directory / "toy.toml")],
            text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.directory / "generated-toy" / "greenmeasurer").is_file())

    def test_over_time_aligns_devices_on_one_csv(self):
        result, rows, events = self.run_engine("-command", "sleep 0.3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0]), [
            "Language", "Program", "Powercap", "Timestamp_ms", "Energy.Package_uJ",
            "Energy.Temperature_C", "Processes.NumberProcesses"])
        self.assertGreater(len(rows), 10)
        self.assertEqual({row["Processes.NumberProcesses"] for row in rows}, {"3"})
        energy = [int(row["Energy.Package_uJ"]) for row in rows]
        self.assertEqual(energy, sorted(energy))
        stamps = [float(row["Timestamp_ms"]) for row in rows]
        self.assertEqual(stamps, sorted(stamps))
        self.assertGreater(stamps[0], 0)
        self.assertEqual(events, ["setup", "teardown"])

    def test_over_time_short_workload_keeps_final_sample_without_zero_row(self):
        result, rows, _ = self.run_engine("-command", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(rows), 1)
        self.assertGreater(float(rows[0]["Timestamp_ms"]), 0)

    def test_standard_reads_at_run_boundaries_without_threads(self):
        result, rows, _ = self.run_engine("--standard", "-n_times", "2", "-command", "sleep 0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0])[3], "Time_ms")
        self.assertEqual(len(rows), 2)
        # The toy temperature advances on every read, so one final read per run.
        self.assertEqual([row["Energy.Temperature_C"] for row in rows], ["40.5", "41.5"])
        for row in rows:
            self.assertEqual(row["Energy.Package_uJ"], "10")
            self.assertEqual(row["Processes.NumberProcesses"], "3")

    def test_metric_flags_select_what_is_measured(self):
        result, rows, events = self.run_engine("--Processes.NumberProcesses", "-command", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0])[4:], ["Processes.NumberProcesses"])
        self.assertEqual(events, [])

    def test_runs_without_metrics(self):
        result, rows, events = self.run_engine("--no-metrics", "-command", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0]), ["Language", "Program", "Powercap", "Timestamp_ms"])
        self.assertEqual(events, [])

    def test_failed_setup_aborts_and_tears_down(self):
        result, rows, _ = self.run_engine("-command", "true", TOY_FAIL_SETUP="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Setup of device 'Energy' failed", result.stderr)
        self.assertEqual(rows, [])

    def test_optional_device_preserves_columns_when_setup_fails(self):
        config = self.directory / "optional-device.toml"
        source = (FIXTURES / "toy.toml").read_text()
        config.write_text(source.replace('name = "Energy"\n', 'name = "Energy"\noptional = true\n', 1))
        executable = self.weave(config, self.directory / "optional-device")
        for mode in ("--standard", "--over-time"):
            with self.subTest(mode=mode):
                result, rows, events = self.run_engine(
                    mode, "-command", "sleep 0.1", executable=executable, TOY_FAIL_SETUP="1")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Setup of optional device 'Energy' failed", result.stderr)
                self.assertTrue(rows)
                self.assertTrue(all(row["Energy.Package_uJ"] == "NOT_SUPPORTED" and
                                    row["Energy.Temperature_C"] == "NOT_SUPPORTED" and
                                    row["Processes.NumberProcesses"] == "3" for row in rows))
                self.assertEqual(events, ["setup"])

                result, rows, events = self.run_engine(mode, "-command", "sleep 0.1",
                                                       executable=executable)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(rows)
                self.assertTrue(all(row["Energy.Package_uJ"] != "NOT_SUPPORTED" and
                                    row["Processes.NumberProcesses"] == "3" for row in rows))
                self.assertEqual(events, ["setup", "teardown"])

    def test_unavailable_metric_is_marked_without_losing_other_readings(self):
        for mode in ("--standard", "--over-time"):
            with self.subTest(mode=mode):
                result, rows, _ = self.run_engine(
                    mode, "-command", "sleep 0.1", TOY_FAIL_TEMPERATURE="1")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(rows)
                self.assertTrue(all(row["Energy.Temperature_C"] == "NOT_SUPPORTED" and
                                    row["Energy.Package_uJ"] != "NOT_SUPPORTED" and
                                    row["Processes.NumberProcesses"] == "3" for row in rows))

    def test_fibonacci_configs_measure_both_gpus_and_report_unavailable_values(self):
        baseline = self.directory / "fibonacci-baseline.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        mock_source = FIXTURES / "fibonacci_devices.c"
        expected = ["IntelCPU.Package_J", "IntelCPU.Core_J", "IntelCPU.DRAM_J",
                    "IntelGPU.Energy_J", "NvidiaGPU.Energy_J", "NvidiaGPU.Temperature_C"]
        replacements = {
            'command = ': 'command = "true"',
            'output = ': f'output = "{self.directory / "fibonacci-result.csv"}"',
            'repetitions = ': 'repetitions = 1',
            'sleep_seconds = ': 'sleep_seconds = 0',
            'powercap = ': 'powercap = [-1]',
            'powercap_min = ': 'powercap_min = 1',
            'powercap_max = ': 'powercap_max = 1',
        }
        for mode in ("standard", "overtime", "powercap-calibration"):
            source = (ROOT / "configurations-generated" /
                      f"fibonacci-python-{mode}.toml").read_text()
            lines = []
            for line in source.splitlines():
                if line.startswith("lib = "):
                    if "libgm_intel_cpu.a" in line:
                        lines.append(f'lib = "{mock_source}"')
                    continue
                if line.startswith("calibration_file = "):
                    line = f'calibration_file = "{baseline}"'
                if line.startswith("header = "):
                    path = line.split('"')[1]
                    line = f'header = "{(ROOT / "configurations-generated" / path).resolve()}"'
                if line.startswith("link = "):
                    continue
                for prefix, replacement in replacements.items():
                    if line.startswith(prefix):
                        line = replacement
                        break
                lines.append(line)
            config = self.directory / f"fibonacci-{mode}.toml"
            config.write_text("\n".join(lines) + "\n")
            executable = self.weave(config, self.directory / f"fibonacci-{mode}")
            output = self.directory / "fibonacci-result.csv"
            cases = (
                ({}, None),
                ({"MOCK_CPU_PACKAGE_UNAVAILABLE": "1"}, "IntelCPU.Package_J"),
                ({"MOCK_INTEL_GPU_UNAVAILABLE": "1"}, "IntelGPU.Energy_J"),
                ({"MOCK_NVIDIA_GPU_UNAVAILABLE": "1"}, "NvidiaGPU.Energy_J"),
                ({"MOCK_NVIDIA_TEMPERATURE_UNAVAILABLE": "1"}, "NvidiaGPU.Temperature_C"),
            )
            for extra_env, unavailable in cases:
                with self.subTest(mode=mode, unavailable=unavailable):
                    output.unlink(missing_ok=True)
                    result = subprocess.run([str(executable)], cwd=self.directory,
                                            env={**os.environ, **extra_env},
                                            text=True, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    with output.open() as measurements:
                        rows = list(csv.DictReader(measurements))
                    self.assertTrue(rows)
                    self.assertEqual(list(rows[0])[4:], expected)
                    for column in expected:
                        if unavailable == "NvidiaGPU.Energy_J" and column == "NvidiaGPU.Temperature_C":
                            self.assertEqual(rows[-1][column], "NOT_SUPPORTED")
                        else:
                            self.assertEqual(rows[-1][column] == "NOT_SUPPORTED", column == unavailable)

    def test_examples_generate_and_run_all_declared_metrics(self):
        mock_source = FIXTURES / "fibonacci_devices.c"
        intel = [
            "IntelCPU.Package_J", "IntelCPU.Core_J", "IntelCPU.DRAM_J", "IntelCPU.Temperature_C",
            "IntelGPU.Energy_J", "IntelGPU.Temperature_C",
        ]
        nvidia = [
            "NvidiaGPU.Energy_J", "NvidiaGPU.Power_W", "NvidiaGPU.Temperature_C",
            "NvidiaGPU.Utilization_%", "NvidiaGPU.VRAM_MiB",
        ]
        system = [
            "System.Cores_cores", "System.CoreFrequency_MHz", "System.RunningProcesses_processes",
            "System.WorkloadProcesses_processes", "System.WorkloadMemory_KiB",
        ]
        groups = {"combined": intel + nvidia + system, "intel": intel,
                  "nvidia": nvidia, "system": system}
        for group, metrics in groups.items():
            folder = ROOT / "examples"
            if group != "combined":
                folder /= group
            baseline = self.directory / f"example-{group}-baseline.txt"
            for mode in ("temperature-calibration", "standard", "overtime", "powercap-calibration"):
                source_path = folder / f"{mode}.toml"
                output = self.directory / f"example-{group}-{mode}.csv"
                lines = []
                injected = False
                for line in source_path.read_text().splitlines():
                    if line.startswith("lib = "):
                        if not injected:
                            lines.append(f'lib = "{mock_source}"')
                            injected = True
                        continue
                    if line.startswith("header = "):
                        path = line.split('"')[1]
                        line = f'header = "{(folder / path).resolve()}"'
                    if line.startswith("link = "):
                        continue
                    if line.startswith("output = "):
                        line = f'output = "{output}"'
                    if line.startswith("calibration_file = "):
                        line = f'calibration_file = "{baseline}"'
                    if line.startswith("sleep_seconds = "):
                        line = "sleep_seconds = 0"
                    lines.append(line)
                config = self.directory / f"example-{group}-{mode}.toml"
                config.write_text("\n".join(lines) + "\n")
                executable = self.weave(config, self.directory / f"example-{group}-{mode}")
                with self.subTest(group=group, mode=mode):
                    result = subprocess.run([str(executable)], cwd=ROOT, text=True,
                                            capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if mode == "temperature-calibration":
                        self.assertIn("Average temperature: 40.0", baseline.read_text())
                    else:
                        with output.open() as measurements:
                            rows = list(csv.DictReader(measurements))
                        self.assertTrue(rows)
                        expected = metrics
                        if mode == "powercap-calibration" and group in ("nvidia", "system"):
                            expected = ["IntelCPU.Package_J"] + metrics
                        self.assertEqual(list(rows[0])[4:], expected)
                        self.assertTrue(all(rows[-1][column] != "NOT_SUPPORTED"
                                            for column in expected))

    def test_capacity_is_a_compile_time_limit(self):
        small = self.weave(self.directory / "toy.toml", self.directory / "small", "--max-samples", "3")
        result, rows, _ = self.run_engine("-command", "sleep 1", executable=small)
        self.assertEqual(result.returncode, 1)
        self.assertIn("capacity (3)", result.stderr)
        self.assertEqual(rows, [])

    def test_power_capping_needs_a_device_with_powercap_hooks(self):
        result, _, _ = self.run_engine("-powercap", "5", "-command", "true")
        self.assertEqual(result.returncode, 1)
        self.assertIn("No device declares powercap_set", result.stderr)

    def test_over_time_applies_and_restores_each_powercap(self):
        result, rows, events = self.run_engine(
            "-powercap", "-1,5", "-n_times", "2", "-command", "true",
            "-calib-file", str(self.directory / "t.txt"),
            executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(events, ["set:-1", "restore", "set:5", "restore"])
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "5", "5"])

    def test_over_time_waits_between_repetitions(self):
        started = time.monotonic()
        result, rows, _ = self.run_engine(
            "-n_times", "2", "-sleep-secs", "1", "-command", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(rows), 2)
        self.assertGreaterEqual(time.monotonic() - started, 1)

    def test_standard_repeats_every_powercap(self):
        result, rows, events = self.run_engine(
            "--standard", "-powercap", "3,4", "-n_times", "2", "-command", "true",
            "-calib-file", str(self.directory / "t.txt"), executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["3", "3", "4", "4"])
        self.assertEqual(events, ["set:3", "restore", "set:4", "restore"])

    def test_powercap_calibration_sweeps_through_run_testing(self):
        result, rows, events = self.run_engine(
            "--powercap-calibration", "-range", "2-3", "-n_times", "2", "-command", "true",
            "-calib-file", str(self.directory / "t.txt"), executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "2", "2", "3", "3"])
        self.assertEqual(events, ["set:-1", "restore", "set:2", "restore", "set:3", "restore"])

    def test_powercap_calibration_waits_between_repetitions(self):
        started = time.monotonic()
        result, rows, _ = self.run_engine(
            "--powercap-calibration", "-range", "2-2", "-n_times", "2", "-sleep-secs", "1",
            "-command", "true", "-calib-file", str(self.directory / "t.txt"),
            executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "2", "2"])
        self.assertGreaterEqual(time.monotonic() - started, 2)

    def test_powercap_calibration_mode_runs_from_toml_without_cli_overrides(self):
        baseline = self.directory / "calibration-baseline.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        output = "configured-powercap-calibration.csv"
        source = (FIXTURES / "toy_hooks.toml").read_text()
        device_config = source.split("[[device]]", 1)[1]
        config = self.directory / "configured-powercap-calibration.toml"
        config.write_text(
            '[measurement]\n'
            'mode = "powercap-calibration"\n'
            'command = "test -f compiled-workload"\n'
            'compile_command = "touch compiled-workload"\n'
            'language = "Shell"\n'
            'program = "true"\n'
            f'output = "{output}"\n'
            'powercap_min = 2\n'
            'powercap_max = 3\n'
            'timeout_hours = 2\n'
            'repetitions = 2\n'
            'sleep_seconds = 0\n'
            f'calibration_file = "{baseline}"\n'
            'variance = 5\n\n'
            '[[device]]' + device_config
        )
        executable = self.weave(config, self.directory / "configured-calibration")
        log = self.directory / "configured-calibration.log"
        result = subprocess.run(
            [str(executable)], cwd=self.directory, text=True, capture_output=True,
            env={**os.environ, "TOY_LOG": str(log), "TOY_LOG_TIMEOUT": "1"}, timeout=60)
        with (self.directory / output).open() as measurements:
            rows = list(csv.DictReader(measurements))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "2", "2", "3", "3"])
        self.assertEqual(log.read_text().split(), [
            "timeout:7200", "set:-1", "restore", "set:2", "restore", "set:3", "restore"])

    def test_powercap_calibration_stops_when_compile_command_fails(self):
        source = (FIXTURES / "toy_hooks.toml").read_text()
        source = source.replace('mode = "over-time"', 'mode = "powercap-calibration"\n'
                                'compile_command = "false"\npowercap_min = 2\npowercap_max = 3')
        source = source.replace("interval_ms = 10\n", "")
        config = self.directory / "compile-failure.toml"
        config.write_text(source)
        executable = self.weave(config, self.directory / "compile-failure")
        result = subprocess.run([str(executable)], cwd=self.directory, text=True,
                                capture_output=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Compilation command failed", result.stderr)

    def test_temperature_calibration_writes_the_baseline(self):
        calibration = self.directory / "baseline.txt"
        calibration.unlink(missing_ok=True)
        result, _, _ = self.run_engine("--temperature-calibration", "-calib-file", str(calibration),
                                       executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Average temperature: 40.0", calibration.read_text())

    def test_temperature_calibration_config_waits_then_writes_baseline(self):
        calibration = self.directory / "configured-temperature.txt"
        calibration.write_text("old baseline\n")
        source = (FIXTURES / "toy_hooks.toml").read_text()
        source = source.replace('mode = "over-time"', 'mode = "temperature-calibration"\n'
                                'sleep_seconds = 1')
        source = source.replace('calibration_file = "toy-temperature-baseline.txt"',
                                f'calibration_file = "{calibration}"')
        config = self.directory / "configured-temperature.toml"
        config.write_text(source)
        executable = self.weave(config, self.directory / "configured-temperature")
        started = time.monotonic()
        result = subprocess.run([str(executable)], cwd=self.directory,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(time.monotonic() - started, 1)
        self.assertIn("Average temperature: 40.0", calibration.read_text())
        self.assertNotIn("old baseline", calibration.read_text())

    def test_temperature_calibration_fails_when_thermal_device_setup_fails(self):
        baseline = self.directory / "failed-temperature.txt"
        baseline.unlink(missing_ok=True)
        source = (FIXTURES / "toy_hooks.toml").read_text()
        source = source.replace('mode = "over-time"', 'mode = "temperature-calibration"')
        source = source.replace('calibration_file = "toy-temperature-baseline.txt"',
                                f'calibration_file = "{baseline}"')
        source = source.replace('thermal = "double toy_thermal()"',
                                'setup = "int toy_setup()"\nthermal = "double toy_thermal()"')
        config = self.directory / "failed-temperature.toml"
        config.write_text(source)
        executable = self.weave(config, self.directory / "failed-temperature")
        result = subprocess.run(
            [str(executable), "--no-metrics"], cwd=self.directory, text=True,
            capture_output=True, env={**os.environ, "TOY_FAIL_SETUP": "1"}, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("temperature calibration cannot run", result.stderr)
        self.assertFalse(baseline.exists())


class WeaverValidationTest(unittest.TestCase):
    def check(self, text, expected):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "d.h").write_text("int f(void);\n")
            config = directory / "c.toml"
            config.write_text(text)
            result = subprocess.run(
                [sys.executable, str(WEAVER), str(config), "--no-build"],
                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(expected, result.stderr)

    DEVICE = ('[measurement]\nmode = "over-time"\n'
              'calibration_file = "temperature-baseline.txt"\n'
              '[[device]]\nname = "D"\nheader = "d.h"\n')

    def test_rejects_devices_inside_measurement(self):
        self.check('[measurement]\nmode = "over-time"\ndevice = "RAPL"\n', "[[device]]")

    def test_requires_calibration_file(self):
        self.check('[measurement]\nmode = "over-time"\n',
                   "measurement.calibration_file is required")

    def test_rejects_unsupported_return_type(self):
        self.check(self.DEVICE + '[[device.metric]]\nname = "M"\nread = "char f()"\n',
                   "unsupported return type")

    def test_rejects_function_with_arguments(self):
        self.check(self.DEVICE + '[[device.metric]]\nname = "M"\nread = "int f(int x)"\n',
                   "must have the form")

    def test_rejects_device_without_metrics(self):
        self.check(self.DEVICE, "at least one")

    def test_rejects_unknown_kind(self):
        self.check(self.DEVICE + '[[device.metric]]\nname = "M"\nread = "int f()"\nkind = "x"\n',
                   "kind must be one of")

    def test_rejects_non_boolean_optional_device(self):
        self.check(self.DEVICE + 'optional = "yes"\n', "optional must be a boolean")

    def test_rejects_powercap_set_without_restore(self):
        self.check(self.DEVICE + 'powercap_set = "int f(int)"\n[[device.metric]]\nname = "M"\n'
                   'read = "int f()"\n', "declared together")

    def test_rejects_invalid_powercap_list(self):
        self.check('[measurement]\nmode = "over-time"\n'
                   'calibration_file = "temperature-baseline.txt"\npowercap = [5, 5]\n', "unique")

    def test_rejects_powercap_calibration_without_range(self):
        self.check('[measurement]\nmode = "powercap-calibration"\ncommand = "true"\n'
                   'calibration_file = "temperature-baseline.txt"\npowercap_min = 2\n'
                   '[[device]]\nname = "D"\nheader = "d.h"\n'
                   '[[device.metric]]\nname = "M"\nread = "int f()"\n',
                   "measurement.powercap_max must be between 1")

    def test_rejects_powercap_calibration_without_device_hook(self):
        self.check('[measurement]\nmode = "powercap-calibration"\ncommand = "true"\n'
                   'calibration_file = "temperature-baseline.txt"\n'
                   'powercap_min = 2\npowercap_max = 3\n[[device]]\nname = "D"\n'
                   'header = "d.h"\n[[device.metric]]\nname = "M"\nread = "int f()"\n',
                   "requires a device declaring powercap_calibration")

    def test_rejects_temperature_calibration_without_thermal_hook(self):
        self.check('[measurement]\nmode = "temperature-calibration"\n'
                   'calibration_file = "temperature-baseline.txt"\n',
                   "temperature-calibration mode requires a device declaring thermal")

    def test_rejects_invalid_timeout(self):
        self.check('[measurement]\nmode = "standard"\n'
                   'calibration_file = "temperature-baseline.txt"\ntimeout_hours = 0\n',
                   "measurement.timeout_hours must be between 1 and 1000")

    def test_rejects_compile_command_outside_calibration(self):
        self.check('[measurement]\nmode = "standard"\n'
                   'calibration_file = "temperature-baseline.txt"\ncompile_command = "true"\n',
                   "measurement.compile_command is only valid")


class WeaverScriptsTest(unittest.TestCase):
    def test_fibonacci_sequence_generates_and_runs_in_order(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            tools = directory / "tools"
            tools.mkdir()
            make = tools / "make"
            make.write_text("#!/bin/sh\nexit 0\n")
            make.chmod(0o755)
            python = tools / "python3"
            python.write_text(
                '#!/bin/sh\n'
                'name=$(basename "$2")\n'
                'printf "generate:%s\\n" "$name" >> "$RUN_LOG"\n'
                'mkdir -p "$4"\n'
                'printf \'#!/bin/sh\\nprintf "run:%%s\\\\n" "%s" >> "$RUN_LOG"\\n'
                '[ "${FAIL_ON:-}" != "%s" ]\\n\' "$name" "$name" > "$4/greenmeasurer"\n'
                'chmod +x "$4/greenmeasurer"\n')
            python.chmod(0o755)
            log = directory / "sequence.log"
            env = {**os.environ, "PATH": f'{tools}{os.pathsep}{os.environ["PATH"]}',
                   "RUN_LOG": str(log)}
            script = ROOT / "scripts" / "run_fibonacci_sequence.sh"
            result = subprocess.run(
                ["bash", str(script), str(directory / "build")], cwd=directory,
                env=env, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            configs = [f"fibonacci-python-{mode}.toml" for mode in (
                "temperature-calibration", "standard", "overtime", "powercap-calibration")]
            self.assertEqual(log.read_text().splitlines(),
                             [f"generate:{config}" for config in configs] +
                             [f"run:{config}" for config in configs])

            log.unlink()
            env["FAIL_ON"] = configs[1]
            result = subprocess.run(
                ["bash", str(script), str(directory / "failed")], cwd=directory,
                env=env, text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(log.read_text().splitlines(),
                             [f"generate:{config}" for config in configs] +
                             [f"run:{config}" for config in configs[:2]])

    def test_batch_generation_and_execution(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            configs = []
            for mode in ("standard", "over-time"):
                config = directory / f"{mode}.toml"
                config.write_text(
                    f'[measurement]\nmode = "{mode}"\n'
                    f'calibration_file = "{directory / "temperature-baseline.txt"}"\n'
                    f'command = "true"\n'
                    f'output = "{directory / (mode + ".csv")}"\n')
                configs.append(config)
            script = ROOT / "scripts" / "measure.sh"
            batch = directory / "batch"
            result = subprocess.run(
                ["bash", str(script), "generate", str(batch), *map(str, configs)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((directory / "standard.csv").exists())
            result = subprocess.run(["bash", str(script), "run", str(batch)],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((directory / "standard.csv").is_file())
            self.assertTrue((directory / "over-time.csv").is_file())
            (batch / "run-0002" / "greenmeasurer").unlink()
            result = subprocess.run(["bash", str(script), "run", str(batch)],
                                    text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("No compiled executable", result.stderr)

            invalid = directory / "invalid.toml"
            invalid.write_text('[measurement]\nmode = "invalid"\n')
            incomplete = directory / "incomplete"
            result = subprocess.run(
                ["bash", str(script), "generate", str(incomplete), str(configs[0]), str(invalid)],
                text=True, capture_output=True, timeout=120)
            self.assertNotEqual(result.returncode, 0)
            result = subprocess.run(["bash", str(script), "run", str(incomplete)],
                                    text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not completely generated", result.stderr)

    def test_calibration_wrapper_rejects_other_modes(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            config = directory / "standard.toml"
            config.write_text('[measurement]\nmode = "standard"\n'
                              'calibration_file = "temperature-baseline.txt"\n'
                              'command = "true"\n')
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_powercap_calibration.sh"),
                 str(config), str(directory / "build")],
                text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("measurement.mode must be 'powercap-calibration'", result.stderr)
            self.assertFalse((directory / "build").exists())

    def test_calibration_wrapper_generates_configured_executable(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            for filename in ("toy.c", "toy.h"):
                shutil.copy(FIXTURES / filename, directory / filename)
            baseline = directory / "baseline.txt"
            baseline.write_text("Average temperature: 40.0 C\n")
            config = directory / "calibration.toml"
            config.write_text(
                (FIXTURES / "toy_hooks.toml").read_text()
                .replace('mode = "over-time"', 'mode = "powercap-calibration"\n'
                         'powercap_min = 2\npowercap_max = 2')
                .replace('calibration_file = "toy-temperature-baseline.txt"',
                         f'calibration_file = "{baseline}"')
                .replace('command = "sleep 0.2"', 'command = "true"')
                .replace("interval_ms = 10\n", "")
                .replace('lib = "libtoy.a"', 'lib = "toy.c"'))
            build = directory / "build"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_powercap_calibration.sh"),
                 str(config), str(build)], text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(build / "greenmeasurer")], cwd=directory,
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((directory / "hooks.csv").is_file())

    def test_run_all_configurations_uses_new_format(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            config = directory / "standard.toml"
            config.write_text('[measurement]\nmode = "standard"\ncommand = "true"\n'
                              'calibration_file = "temperature-baseline.txt"\n'
                              'output = "measurements.csv"\n')
            results = directory / "results"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_all_configurations.sh"),
                 "-o", str(results), str(config)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("PASS: 1  WARN: 0  FAIL: 0", result.stdout)
            self.assertEqual(len(list(results.rglob("measurements.csv"))), 1)

    def test_run_all_reports_warnings_and_continues_after_failure(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "metric.h").write_text("int metric(void);\n")
            (directory / "metric.c").write_text("int metric(void) { return -1; }\n")
            (directory / "a-invalid.toml").write_text('[measurement]\nmode = "invalid"\n')
            (directory / "b-warning.toml").write_text(
                '[measurement]\nmode = "standard"\ncommand = "true"\n'
                'calibration_file = "baseline.txt"\noutput = "measurements.csv"\n'
                '[[device]]\nname = "Toy"\nheader = "metric.h"\nlib = "metric.c"\n'
                '[[device.metric]]\nname = "Value"\nread = "int metric()"\n')
            results = directory / "results"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_all_configurations.sh"),
                 "-o", str(results), str(directory)],
                text=True, capture_output=True, timeout=120)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("PASS: 0  WARN: 1  FAIL: 1", result.stdout)
            summary = (results / "summary.txt").read_text()
            self.assertIn("FAIL  ", summary)
            self.assertIn("WARN  ", summary)
            self.assertIn("NOT_SUPPORTED", next(results.rglob("measurements.csv")).read_text())

    def test_examples_wrapper_forwards_options_and_examples_directory(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            bash = directory / "bash"
            bash.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGS_LOG"\nexit 23\n')
            bash.chmod(0o755)
            log = directory / "arguments.txt"
            result = subprocess.run(
                ["/bin/bash", str(ROOT / "scripts" / "test_examples.sh"), "-o", "results"],
                env={**os.environ, "PATH": f'{directory}{os.pathsep}{os.environ["PATH"]}',
                     "ARGS_LOG": str(log)},
                text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 23)
            self.assertEqual(log.read_text().splitlines(), [
                str(ROOT / "scripts" / "run_all_configurations.sh"),
                "-o", "results", str(ROOT / "examples")])


if __name__ == "__main__":
    unittest.main()
