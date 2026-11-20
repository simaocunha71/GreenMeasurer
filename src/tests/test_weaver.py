import csv
import ctypes.util
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
        binary = executable or self.executable
        modes = ("--aggregate", "--over-time", "--temperature-calibration", "--powercap-calibration")
        mode = next((arg for arg in arguments if arg in modes), "--over-time")
        chosen = [arg for arg in arguments if arg.startswith("--") and arg not in modes]
        supplied_chosen = bool(chosen)
        if not chosen:
            chosen = (["--Cpu.Temperature"] if mode == "--temperature-calibration" else
                      ["--Cpu.Package", "--Cpu.Temperature", "--Cpu.Powercap"] if binary == self.hooks else
                      ["--Energy.Package", "--Energy.Gauge", "--Processes.NumberProcesses"])
        defaults = [] if any(arg in modes for arg in arguments) else [mode]
        defaults += ["-sleep-secs", "0"]
        if mode != "--temperature-calibration":
            defaults += ["-command", "sleep 0.3", "-language", "Shell", "-program", "sleep",
                         "-output", str(output), "-n_times", "1", "-time_out_limit", "0"]
            if mode != "--powercap-calibration":
                defaults += ["-powercap", "-1"]
            if mode == "--over-time":
                defaults += ["-interval-ms", "10"]
        if "--Cpu.Temperature" in chosen:
            if "-calib-file" not in arguments:
                defaults += ["-calib-file", f"Cpu.Temperature={self.directory / 't.txt'}"]
            if mode != "--temperature-calibration":
                defaults += ["-temperature-control", "Cpu.Temperature", "-variance", "5"]
        result = subprocess.run(
            [str(binary), *defaults, *([] if supplied_chosen else chosen), *arguments],
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

    def test_without_aspects_runs_system_without_measurements(self):
        config = self.directory / "plain.toml"
        config.write_text("")
        executable = self.weave(config, self.directory / "plain")
        generated = (self.directory / "plain" / "GreenMeasurer.c").read_text()
        self.assertIn("system(argv[2])", generated)
        compile_command = subprocess.run(
            [sys.executable, str(WEAVER), str(config), "--no-build",
             "-o", str(self.directory / "plain-source")],
            text=True, capture_output=True, timeout=30)
        self.assertEqual(compile_command.returncode, 0, compile_command.stderr)
        self.assertNotIn("gm_runtime.c", compile_command.stdout)
        self.assertNotIn("-pthread", compile_command.stdout)
        result = subprocess.run([str(executable), "-command", "printf executed"], cwd=self.directory,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "executed")
        self.assertFalse((self.directory / "measurements.csv").exists())
        result = subprocess.run([str(executable)], cwd=self.directory,
                                text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("A command is required", result.stderr)
        result = subprocess.run([str(executable), "-command", "exit 7"], cwd=self.directory,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 7)
        self.assertIn("status 7", result.stderr)

    def test_over_time_aligns_devices_on_one_csv(self):
        result, rows, events = self.run_engine("-command", "sleep 0.3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0]), [
            "Language", "Program", "Powercap", "Timestamp_ms", "Energy.Package_uJ",
            "Energy.Gauge_C", "Processes.NumberProcesses"])
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

    def test_aggregate_reads_at_run_boundaries_without_threads(self):
        result, rows, _ = self.run_engine("--aggregate", "-n_times", "2", "-command", "sleep 0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0])[3], "Energy.Package_uJ")
        self.assertEqual(len(rows), 2)
        # The toy temperature advances on every read, so one final read per run.
        self.assertEqual([row["Energy.Gauge_C"] for row in rows], ["40.5", "41.5"])
        for row in rows:
            self.assertEqual(row["Energy.Package_uJ"], "10")
            self.assertEqual(row["Processes.NumberProcesses"], "3")

    def test_only_over_time_starts_sampling_threads(self):
        source = self.directory / "deny-threads.c"
        library = self.directory / "deny-threads.so"
        source.write_text(
            '#include <pthread.h>\n#include <errno.h>\n'
            'int pthread_create(pthread_t *t, const pthread_attr_t *a, '
            'void *(*start)(void *), void *arg) { return EAGAIN; }\n')
        subprocess.run(["gcc", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
        aggregate, rows, _ = self.run_engine("--aggregate", "-command", "true",
                                            LD_PRELOAD=str(library))
        self.assertEqual(aggregate.returncode, 0, aggregate.stderr)
        self.assertTrue(rows)
        overtime, _, _ = self.run_engine("--over-time", "-command", "sleep 0.1",
                                         LD_PRELOAD=str(library))
        self.assertNotEqual(overtime.returncode, 0)
        self.assertIn("Failed to start the sampling thread", overtime.stderr)

    def test_metric_flags_select_what_is_measured(self):
        result, rows, events = self.run_engine("--Processes.NumberProcesses", "-command", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(rows[0])[4:], ["Processes.NumberProcesses"])
        self.assertEqual(events, [])

    def test_runs_without_metrics(self):
        result, rows, events = self.run_engine("--no-metrics", "-command", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(rows, [])
        self.assertEqual(events, [])

    def test_plain_execution_when_no_aspect_is_active(self):
        result = subprocess.run([str(self.executable), "-command", "exit 7"],
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 7)
        self.assertIn("status 7", result.stderr)
        marker = self.directory / "plain-runs.txt"
        marker.unlink(missing_ok=True)
        result, rows, events = self.run_engine(
            "--aggregate", "--Energy.Package", "-n_times", "3",
            "-command", f"echo run >> {marker}", TOY_FAIL_SETUP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("continuing without it", result.stderr)
        self.assertEqual(marker.read_text().splitlines(), ["run"])
        self.assertEqual(rows, [])
        self.assertEqual(events, ["setup"])
        marker.unlink()
        result, rows, events = self.run_engine(
            "--aggregate", "--no-metrics", "-n_times", "3",
            "-command", f"echo run >> {marker}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No active aspects", result.stderr)
        self.assertEqual(marker.read_text().splitlines(), ["run"])
        self.assertEqual(rows, [])
        self.assertEqual(events, [])

    def test_powercap_setup_failure_omits_control_but_calibration_fails(self):
        config = self.directory / "failed-control.toml"
        config.write_text((FIXTURES / "toy.toml").read_text() +
                          '\n[[aspect]]\nname = "Cpu.Powercap"\nlib = "libtoy.a"\n'
                          'header = "toy.h"\nsetup = "int toy_setup()"\n'
                          'teardown = "void toy_teardown()"\n'
                          'powercap_set = "int toy_cap_set(int)"\n'
                          'powercap_restore = "int toy_cap_restore()"\n'
                          'powercap_calibration = "toy_calibrate"\n')
        executable = self.weave(config, self.directory / "failed-control")
        result, rows, events = self.run_engine(
            "--aggregate", "--Processes.NumberProcesses", "--Cpu.Powercap", "-powercap", "5",
            "-command", "true", executable=executable, TOY_FAIL_SETUP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Powercap limits will not be applied", result.stderr)
        self.assertEqual(rows[0]["Powercap"], "-1")
        self.assertEqual(rows[0]["Processes.NumberProcesses"], "3")
        self.assertEqual(events, ["setup"])
        result, rows, events = self.run_engine(
            "--powercap-calibration", "--Processes.NumberProcesses", "--Cpu.Powercap",
            "-range", "1-1", "-command", "true", executable=executable, TOY_FAIL_SETUP="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("calibration cannot proceed", result.stderr)
        self.assertEqual(rows, [])
        self.assertEqual(events, ["setup"])

    def test_failed_shared_setup_omits_aspects_and_keeps_other_metrics(self):
        for mode in ("--aggregate", "--over-time"):
            with self.subTest(mode=mode):
                result, rows, events = self.run_engine(
                    mode, "-command", "sleep 0.1", TOY_FAIL_SETUP="1")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Setup of aspect 'Energy.Package' failed", result.stderr)
                self.assertIn("Setup of aspect 'Energy.Gauge' failed", result.stderr)
                self.assertTrue(rows)
                self.assertTrue(all("Energy.Package_uJ" not in row and
                                    "Energy.Gauge_C" not in row and
                                    row["Processes.NumberProcesses"] == "3" for row in rows))
                self.assertEqual(events, ["setup"])

    def test_unavailable_metric_is_marked_without_losing_other_readings(self):
        for mode in ("--aggregate", "--over-time"):
            with self.subTest(mode=mode):
                result, rows, _ = self.run_engine(
                    mode, "-command", "sleep 0.1", TOY_FAIL_TEMPERATURE="1")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(rows)
                self.assertTrue(all(row["Energy.Gauge_C"] == "NOT_SUPPORTED" and
                                    row["Energy.Package_uJ"] != "NOT_SUPPORTED" and
                                    row["Processes.NumberProcesses"] == "3" for row in rows))

    def test_fibonacci_config_runs_all_modes_and_reports_unavailable_values(self):
        baseline = self.directory / "fibonacci-baseline.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        mock_source = FIXTURES / "fibonacci_devices.c"
        expected = ["IntelCPU.Package_J", "IntelCPU.Core_J", "IntelCPU.DRAM_J",
                    "IntelGPU.Energy_J", "NvidiaGPU.Energy_J", "NvidiaGPU.Temperature_C"]
        source = (ROOT / "configurations-generated" / "config.toml").read_text()
        lines = []
        for line in source.splitlines():
            if line.startswith("lib = "):
                if "wall_clock_time.c" in line:
                    lines.append(f'lib = "{ROOT / "src/aspects/wall_clock_time.c"}"')
                elif any(lib in line for lib in ("libgm_intel_cpu.a", "libgm_intel_cpu_powercap.a",
                                                 "libgm_intel_cpu_temperature.a")):
                    lines.append(f'lib = "{mock_source}"')
                continue
            if line.startswith("header = "):
                path = line.split('"')[1]
                line = f'header = "{(ROOT / "configurations-generated" / path).resolve()}"'
            if line.startswith("link = "):
                continue
            lines.append(line)
        config = self.directory / "fibonacci.toml"
        config.write_text("\n".join(lines) + "\n")
        executable = self.weave(config, self.directory / "fibonacci")
        output = self.directory / "fibonacci-result.csv"
        for aspect, temperature in (("IntelCPU.Temperature", "40.0"),
                                    ("NvidiaGPU.Temperature", "55.0")):
            calibration = self.directory / f"{aspect}.txt"
            result = subprocess.run(
                [str(executable), "--temperature-calibration", "-sleep-secs", "0",
                 f"--{aspect}", "-calib-file", f"{aspect}={calibration}"],
                cwd=self.directory, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"Average temperature: {temperature}", calibration.read_text())
        for mode in ("aggregate", "overtime", "powercap-calibration"):
            cases = (
                ({}, None),
                ({"MOCK_CPU_PACKAGE_UNAVAILABLE": "1"}, "IntelCPU.Package_J"),
                ({"MOCK_INTEL_GPU_UNAVAILABLE": "1"}, "IntelGPU.Energy_J"),
            )
            for extra_env, unavailable in cases:
                with self.subTest(mode=mode, unavailable=unavailable):
                    output.unlink(missing_ok=True)
                    arguments = [
                        "--aggregate" if mode == "aggregate" else
                        "--over-time" if mode == "overtime" else "--powercap-calibration",
                        "-command", "true", "-language", "Shell", "-program", "true",
                        "-output", str(output), "-n_times", "1", "-sleep-secs", "0",
                        "-time_out_limit", "0",
                        "--IntelCPU.Package", "--IntelCPU.Core", "--IntelCPU.DRAM", "--IntelCPU.Powercap",
                        "--IntelGPU.Energy", "--NvidiaGPU.Energy", "--NvidiaGPU.Temperature",
                        "-temperature-control", "NvidiaGPU.Temperature",
                        "-calib-file", f"NvidiaGPU.Temperature={baseline}", "-variance", "100",
                    ]
                    arguments += (["-range", "1-1"] if mode == "powercap-calibration" else
                                  ["-powercap", "-1"] +
                                  (["-interval-ms", "5"] if mode == "overtime" else []))
                    result = subprocess.run([str(executable), *arguments], cwd=self.directory,
                                            env={**os.environ, **extra_env},
                                            text=True, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    with output.open() as measurements:
                        rows = list(csv.DictReader(measurements))
                    self.assertTrue(rows)
                    columns = [column for column in expected
                               if not (unavailable == "IntelGPU.Energy_J" and column == unavailable)]
                    self.assertEqual(list(rows[0])[4 if mode == "overtime" else 3:], columns)
                    for column in columns:
                        self.assertEqual(rows[-1][column] == "NOT_SUPPORTED", column == unavailable)
            for variable in ("MOCK_NVIDIA_GPU_UNAVAILABLE", "MOCK_NVIDIA_TEMPERATURE_UNAVAILABLE"):
                with self.subTest(mode=mode, unavailable=variable):
                    output.unlink(missing_ok=True)
                    result = subprocess.run([str(executable), *arguments], cwd=self.directory,
                                            env={**os.environ, variable: "1"},
                                            text=True, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("temperature aspect", result.stderr.lower())
                    with output.open() as measurements:
                        rows = list(csv.DictReader(measurements))
                    self.assertTrue(rows)
                    if variable == "MOCK_NVIDIA_GPU_UNAVAILABLE":
                        self.assertNotIn("NvidiaGPU.Temperature_C", rows[-1])
                    else:
                        self.assertEqual(rows[-1]["NvidiaGPU.Temperature_C"], "NOT_SUPPORTED")

    def test_examples_generate_and_run_all_declared_metrics(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        from weave import tomllib

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
                  "nvidia": nvidia, "system": ["Time.WallClock_ms"] + system}
        for group, metrics in groups.items():
            folder = ROOT / "examples"
            if group != "combined":
                folder /= group
            source_path = folder / "config.toml"
            with source_path.open("rb") as source:
                declarations = tomllib.load(source)["aspect"]
            if group in ("nvidia", "system"):
                self.assertFalse(any(aspect["name"].startswith("Intel") for aspect in declarations))
            lines = []
            injected = False
            for line in source_path.read_text().splitlines():
                if line.startswith("lib = "):
                    if "wall_clock_time.c" in line:
                        lines.append(f'lib = "{ROOT / "src/aspects/wall_clock_time.c"}"')
                    elif not injected:
                        lines.append(f'lib = "{mock_source}"')
                        injected = True
                    continue
                if line.startswith("header = "):
                    path = line.split('"')[1]
                    line = f'header = "{(folder / path).resolve()}"'
                if line.startswith("link = "):
                    continue
                lines.append(line)
            config = self.directory / f"example-{group}.toml"
            config.write_text("\n".join(lines) + "\n")
            executable = self.weave(config, self.directory / f"example-{group}")
            thermal = [aspect for aspect in declarations if aspect.get("thermal")]
            controls = [aspect for aspect in declarations if "powercap_calibration" in aspect]
            modes = ["aggregate", "overtime"]
            if thermal:
                modes.append("temperature-calibration")
            if controls:
                modes.append("powercap-calibration")
            for mode in modes:
                with self.subTest(group=group, mode=mode):
                    output = self.directory / f"example-{group}-{mode}.csv"
                    arguments = ["--" + ({"overtime": "over-time", "aggregate": "aggregate"}.get(mode, mode)),
                                 "-sleep-secs", "0"]
                    selected = thermal if mode == "temperature-calibration" else declarations
                    baselines = []
                    for aspect in selected:
                        arguments.append("--" + aspect["name"])
                        if aspect.get("thermal"):
                            baseline = self.directory / f'{group}-{mode}-{aspect["name"]}.txt'
                            if mode != "temperature-calibration":
                                baseline.write_text("Average temperature: 40.0 C\n")
                            baselines.append((aspect["name"], baseline))
                            arguments += ["-calib-file", f'{aspect["name"]}={baseline}']
                            if mode != "temperature-calibration":
                                arguments += ["-temperature-control", aspect["name"]]
                    if mode != "temperature-calibration":
                        arguments += ["-command", "true", "-language", "Shell", "-program", "true",
                                      "-output", str(output), "-n_times", "1", "-time_out_limit", "0"]
                        if any(aspect.get("thermal") for aspect in declarations):
                            arguments += ["-variance", "100"]
                        if mode == "powercap-calibration":
                            arguments += ["-range", "1-1"]
                        elif controls:
                            arguments += ["-powercap", "-1"]
                        if mode == "overtime":
                            arguments += ["-interval-ms", "5"]
                    result = subprocess.run([str(executable), *arguments], cwd=ROOT, text=True,
                                            capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if mode == "temperature-calibration":
                        temperatures = {"IntelCPU.Temperature": "40.0",
                                        "IntelGPU.Temperature": "45.0",
                                        "NvidiaGPU.Temperature": "55.0"}
                        for aspect, baseline in baselines:
                            self.assertIn(f"Average temperature: {temperatures[aspect]}",
                                          baseline.read_text())
                    else:
                        with output.open() as measurements:
                            rows = list(csv.DictReader(measurements))
                        self.assertTrue(rows)
                        self.assertEqual(list(rows[0])[4 if mode == "overtime" else 3:], metrics)
                        self.assertTrue(all(rows[-1][column] != "NOT_SUPPORTED"
                                            for column in metrics))

    def test_capacity_is_a_compile_time_limit(self):
        small = self.weave(self.directory / "toy.toml", self.directory / "small", "--max-samples", "3")
        result, rows, _ = self.run_engine("-command", "sleep 1", executable=small)
        self.assertEqual(result.returncode, 1)
        self.assertIn("capacity (3)", result.stderr)
        self.assertEqual(rows, [])

    def test_power_capping_needs_a_device_with_powercap_hooks(self):
        result, _, _ = self.run_engine("-powercap", "5", "-command", "true")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Select a powercap control aspect", result.stderr)

    def test_over_time_applies_and_restores_each_powercap(self):
        result, rows, events = self.run_engine(
            "-powercap", "-1,5", "-n_times", "2", "-command", "true",
            "-calib-file", f"Cpu.Temperature={self.directory / 't.txt'}",
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

    def test_aggregate_repeats_every_powercap(self):
        result, rows, events = self.run_engine(
            "--aggregate", "-powercap", "3,4", "-n_times", "2", "-command", "true",
            "-calib-file", f"Cpu.Temperature={self.directory / 't.txt'}", executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["3", "3", "4", "4"])
        self.assertEqual(events, ["set:3", "restore", "set:4", "restore"])

    def test_powercap_calibration_sweeps_through_run_testing(self):
        result, rows, events = self.run_engine(
            "--powercap-calibration", "-range", "2-3", "-n_times", "2", "-command", "true",
            "-calib-file", f"Cpu.Temperature={self.directory / 't.txt'}", executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "2", "2", "3", "3"])
        self.assertEqual(events, ["set:-1", "restore", "set:2", "restore", "set:3", "restore"])

    def test_powercap_calibration_waits_between_repetitions(self):
        started = time.monotonic()
        result, rows, _ = self.run_engine(
            "--powercap-calibration", "-range", "2-2", "-n_times", "2", "-sleep-secs", "1",
            "-command", "true", "-calib-file", f"Cpu.Temperature={self.directory / 't.txt'}",
            executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "2", "2"])
        self.assertGreaterEqual(time.monotonic() - started, 2)

    def test_powercap_calibration_uses_explicit_cli_options(self):
        baseline = self.directory / "calibration-baseline.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        output = "configured-powercap-calibration.csv"
        log = self.directory / "configured-calibration.log"
        result = subprocess.run(
            [str(self.hooks), "--powercap-calibration", "-command", "test -f compiled-workload",
             "-compile_cmd", "touch compiled-workload", "-language", "Shell", "-program", "true",
             "-output", output, "-range", "2-3", "-time_out_limit", "2", "-n_times", "2",
             "-sleep-secs", "0", "--Cpu.Package", "--Cpu.Temperature", "--Cpu.Powercap",
             "-temperature-control", "Cpu.Temperature",
             "-calib-file", f"Cpu.Temperature={baseline}", "-variance", "5"],
            cwd=self.directory, text=True, capture_output=True,
            env={**os.environ, "TOY_LOG": str(log), "TOY_LOG_TIMEOUT": "1"}, timeout=60)
        with (self.directory / output).open() as measurements:
            rows = list(csv.DictReader(measurements))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([row["Powercap"] for row in rows], ["-1", "-1", "2", "2", "3", "3"])
        self.assertEqual(log.read_text().split(), [
            "timeout:7200", "set:-1", "restore", "set:2", "restore", "set:3", "restore"])

    def test_powercap_calibration_stops_when_compile_command_fails(self):
        result, _, _ = self.run_engine("--powercap-calibration", "-range", "2-3",
                                       "-compile_cmd", "false", executable=self.hooks)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Compilation command failed", result.stderr)

    def test_temperature_calibration_writes_the_baseline(self):
        calibration = self.directory / "baseline.txt"
        calibration.unlink(missing_ok=True)
        result, _, _ = self.run_engine("--temperature-calibration",
                                       "-calib-file", f"Cpu.Temperature={calibration}",
                                       executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Average temperature: 40.0", calibration.read_text())

    def test_temperature_calibration_waits_then_writes_baseline(self):
        calibration = self.directory / "configured-temperature.txt"
        calibration.write_text("old baseline\n")
        started = time.monotonic()
        result, _, _ = self.run_engine("--temperature-calibration", "-sleep-secs", "1",
                                       "-calib-file", f"Cpu.Temperature={calibration}",
                                       executable=self.hooks)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(time.monotonic() - started, 1)
        self.assertIn("Average temperature: 40.0", calibration.read_text())
        self.assertNotIn("old baseline", calibration.read_text())

    def test_temperature_calibration_fails_when_thermal_device_setup_fails(self):
        baseline = self.directory / "failed-temperature.txt"
        baseline.unlink(missing_ok=True)
        source = (FIXTURES / "toy_hooks.toml").read_text()
        source = source.replace('name = "Cpu.Temperature"',
                                'name = "Cpu.Temperature"\nsetup = "int toy_setup()"')
        config = self.directory / "failed-temperature.toml"
        config.write_text(source)
        executable = self.weave(config, self.directory / "failed-temperature")
        result = subprocess.run(
            [str(executable), "--temperature-calibration", "--Cpu.Temperature",
             "-sleep-secs", "0", "-calib-file", f"Cpu.Temperature={baseline}"],
            cwd=self.directory, text=True,
            capture_output=True, env={**os.environ, "TOY_FAIL_SETUP": "1"}, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Setup of aspect 'Cpu.Temperature' failed", result.stderr)
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

    DEVICE = '[[aspect]]\nname = "D"\nheader = "d.h"\n'

    def test_rejects_measurement_table(self):
        self.check('[measurement]\nmode = "aggregate"\n', "unknown top-level keys")

    def test_rejects_old_device_tables(self):
        self.check('[[device]]\nname = "D"\n', "unknown top-level keys")

    def test_rejects_unsupported_return_type(self):
        self.check(self.DEVICE + 'read = "char f()"\n',
                   "unsupported return type")

    def test_rejects_function_with_arguments(self):
        self.check(self.DEVICE + 'read = "int f(int x)"\n',
                   "must have the form")

    def test_rejects_aspect_without_metric_reader(self):
        self.check(self.DEVICE, ".read must be a string")

    def test_rejects_unknown_kind(self):
        self.check(self.DEVICE + 'read = "int f()"\nkind = "x"\n',
                   "kind must be one of")
        self.check(self.DEVICE + 'read = "int f()"\nkind = []\n',
                   "kind must be one of")

    def test_rejects_removed_optional_field(self):
        self.check(self.DEVICE + 'read = "int f()"\noptional = true\n', "unknown keys")

    def test_rejects_powercap_set_without_restore(self):
        self.check(self.DEVICE + 'powercap_set = "int f(int)"\nread = "int f()"\n',
                   "declared together")

    def test_rejects_powercap_hooks_on_metric_and_metric_fields_on_control(self):
        hooks = 'powercap_set = "int set(int)"\npowercap_restore = "int restore()"\n'
        self.check(self.DEVICE + hooks + 'read = "int f()"\n', "separate control aspect")
        self.check(self.DEVICE + hooks + 'unit = "W"\n', "cannot declare metric fields")

    def test_duration_requires_paired_measurement_callbacks(self):
        self.check(self.DEVICE + 'read = "double f()"\nkind = "duration"\n',
                   "requires measurement_start")
        self.check(self.DEVICE + 'read = "double f()"\nmeasurement_start = "int begin()"\n',
                   "must be declared together")

    def test_rejects_old_nested_metric_tables(self):
        self.check(self.DEVICE + '[[aspect.metric]]\nname = "M"\nread = "int f()"\n',
                   "unknown keys")

    def test_rejects_invalid_temperature_marker(self):
        self.check(self.DEVICE + 'read = "int f()"\nthermal = "int f()"\n',
                   "thermal must be a boolean")
        self.check(self.DEVICE + 'read = "int f()"\nthermal = true\nkind = "counter"\n',
                   "requires kind")

    def test_rejects_reserved_flags_and_invalid_link(self):
        self.check('[[aspect]]\nname = "aggregate"\nheader = "d.h"\nread = "int f()"\n',
                   "conflicts with a built-in")
        self.check(self.DEVICE + 'read = "int f()"\nlink = ""\n', "link must be an array")

    def test_rejects_conflicting_c_symbols_and_lifecycle(self):
        self.check(self.DEVICE + 'read = "int f()"\n'
                   '[[aspect]]\nname = "D"\nheader = "d.h"\nread = "int f()"\n',
                   "duplicate aspect name")
        self.check('[[aspect]]\nname = "A.B"\nheader = "d.h"\nread = "int f()"\n'
                   '[[aspect]]\nname = "A_B"\nheader = "d.h"\nread = "int f()"\n',
                   "collides")
        self.check(self.DEVICE + 'read = "int f()"\nsetup = "int init()"\n'
                   '[[aspect]]\nname = "E"\nheader = "d.h"\nread = "int f()"\n'
                   'setup = "int init()"\nteardown = "void end()"\n',
                   "different teardown")


class WeaverBuildIsolationTest(unittest.TestCase):
    def test_skips_missing_and_broken_aspects_and_keeps_available_metrics(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "values.h").write_text("int good(void);\nint missing(void);\n")
            (directory / "good.c").write_text("int good(void) { return 7; }\n")
            (directory / "broken.c").write_text("int invalid( {\n")
            config = directory / "config.toml"
            config.write_text(
                '[[aspect]]\nname = "Bad.Header"\nheader = "missing.h"\nread = "int missing()"\n'
                '[[aspect]]\nname = "Bad.Library"\nheader = "values.h"\nlib = "missing.a"\n'
                'read = "int missing()"\n'
                '[[aspect]]\nname = "Bad.Source"\nheader = "values.h"\nlib = "broken.c"\n'
                'read = "int missing()"\n'
                '[[aspect]]\nname = "Bad.Symbol"\nheader = "values.h"\nread = "int missing()"\n'
                '[[aspect]]\nname = "Bad.Link"\nheader = "values.h"\nlib = "good.c"\n'
                'link = ["-lgreenmeasurer_missing_test_library"]\nread = "int good()"\n'
                '[[aspect]]\nname = "Good.Value"\nheader = "values.h"\nlib = "good.c"\n'
                'read = "int good()"\n')
            build = directory / "build"
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "-o", str(build)],
                                    text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            for aspect in ("Bad.Header", "Bad.Library", "Bad.Source", "Bad.Symbol", "Bad.Link"):
                self.assertIn(f"Skipping aspect '{aspect}'", result.stderr)
            self.assertIn("missing.a", result.stderr)
            self.assertIn("undefined reference", result.stderr)
            executable = build / "greenmeasurer"
            help_output = subprocess.run([str(executable), "--help"], check=True,
                                         text=True, capture_output=True).stdout
            self.assertIn("--Good.Value", help_output)
            self.assertNotIn("--Bad.", help_output)
            output = directory / "result.csv"
            result = subprocess.run(
                [str(executable), "--aggregate", "-command", "true", "-language", "Shell",
                 "-program", "true", "-output", str(output), "-n_times", "1",
                 "-sleep-secs", "0", "-time_out_limit", "0", "--Good.Value"],
                text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open() as source:
                self.assertEqual(next(csv.DictReader(source))["Good.Value"], "7")
            result = subprocess.run([str(executable), "--Bad.Symbol"], text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unknown", result.stderr)

    def test_retries_aspect_resolved_by_later_library(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "values.h").write_text("int first(void);\nint second(void);\n")
            (directory / "values.c").write_text(
                "int first(void) { return 1; }\nint second(void) { return 2; }\n")
            config = directory / "config.toml"
            config.write_text(
                '[[aspect]]\nname = "First"\nheader = "values.h"\nread = "int first()"\n'
                '[[aspect]]\nname = "Broken"\nheader = "values.h"\nread = "int absent()"\n'
                '[[aspect]]\nname = "Second"\nheader = "values.h"\nlib = "values.c"\n'
                'read = "int second()"\n')
            build = directory / "build"
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "-o", str(build)],
                                    text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Skipping aspect 'Broken'", result.stderr)
            self.assertNotIn("Skipping aspect 'First'", result.stderr)
            help_output = subprocess.run([str(build / "greenmeasurer"), "--help"], check=True,
                                         text=True, capture_output=True).stdout
            self.assertIn("--First", help_output)
            self.assertIn("--Second", help_output)

    def test_all_unbuildable_aspects_fall_back_to_core(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            config = directory / "config.toml"
            config.write_text('[[aspect]]\nname = "Missing"\nheader = "missing.h"\n'
                              'read = "int metric()"\n')
            build = directory / "build"
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "-o", str(build)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Skipping aspect 'Missing'", result.stderr)
            result = subprocess.run([str(build / "greenmeasurer"), "-command", "printf core"],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "core")
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "--no-build",
                                     "-o", str(directory / "source")],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Skipping aspect 'Missing'", result.stderr)
            self.assertNotIn("gm_runtime.c", result.stdout)

    def test_global_compiler_failure_is_not_treated_as_an_aspect_failure(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "metric.h").write_text("int metric(void);\n")
            config = directory / "config.toml"
            config.write_text('[[aspect]]\nname = "Value"\nheader = "metric.h"\n'
                              'read = "int metric()"\n')
            result = subprocess.run([sys.executable, str(WEAVER), str(config),
                                     "-o", str(directory / "build")],
                                    env={**os.environ, "CC": "false"},
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("compilation failed", result.stderr)
            self.assertNotIn("Skipping aspect", result.stderr)


class IntelLibraryIsolationTest(unittest.TestCase):
    def test_intel_readers_link_without_powercap_or_sensors(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            intel = ROOT / "src/devices/linux/cpu/intel"
            includes = ["-I", str(ROOT / "src/include"), "-I", str(ROOT / "src/devices/linux")]
            for source, output in ((intel / "intel_cpu.c", directory / "cpu.o"),
                                   (ROOT / "src/devices/linux/msr.c", directory / "msr.o")):
                subprocess.run(["gcc", *includes, "-c", str(source), "-o", str(output)],
                               check=True, capture_output=True, text=True)
            cpu_lib = directory / "libcpu.a"
            subprocess.run(["ar", "rcs", str(cpu_lib), str(directory / "cpu.o"),
                            str(directory / "msr.o")], check=True)
            config = directory / "intel.toml"
            metric = (
                '[[aspect]]\nname = "IntelCPU.Package"\n'
                f'header = "{intel / "intel_cpu.h"}"\nlib = "{cpu_lib}"\n'
                'setup = "int intel_cpu_begin()"\n'
                'teardown = "void intel_cpu_end()"\nread = "double intel_cpu_get_package()"\n')
            config.write_text(metric)
            build = directory / "metrics-only"
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "-o", str(build)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.run(["nm", str(build / "greenmeasurer")], check=True,
                                     text=True, capture_output=True).stdout
            self.assertNotIn("intel_cpu_set_powercap", symbols)
            self.assertNotIn("raplcap_init", symbols)
            self.assertNotIn("sensors_init", symbols)
            self.assertNotIn("intel_cpu_get_temperature", symbols)
            result = subprocess.run(
                [str(build / "greenmeasurer"), "--aggregate", "-command", "true",
                 "-language", "Shell", "-program", "true", "-output", str(directory / "out.csv"),
                 "-n_times", "1", "-sleep-secs", "0", "-powercap", "-1", "-time_out_limit", "0",
                 "--no-metrics"], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(
        Path("/usr/include/sensors/sensors.h").is_file() and ctypes.util.find_library("sensors"),
        "libsensors development files are unavailable",
    )
    def test_temperature_initializes_sensors_only_when_selected(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            intel = ROOT / "src/devices/linux/cpu/intel"
            source = directory / "mock_sensors.c"
            source.write_text(
                '#include <sensors/sensors.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
                'static void log_event(const char *event) {\n'
                '  FILE *fp = fopen(getenv("SENSORS_LOG"), "a");\n'
                '  if (fp) { fprintf(fp, "%s\\n", event); fclose(fp); }\n}\n'
                'int sensors_init(FILE *input) { (void)input; log_event("init"); '
                'return getenv("FAIL_SENSORS") ? -1 : 0; }\n'
                'void sensors_cleanup(void) { log_event("cleanup"); }\n'
                'const sensors_chip_name *sensors_get_detected_chips(const sensors_chip_name *match, int *nr) '
                '{ (void)match; (void)nr; log_event("read"); return NULL; }\n')
            library = directory / "mock_sensors.so"
            subprocess.run(["gcc", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
            config = directory / "temperature.toml"
            config.write_text(
                '[[aspect]]\nname = "IntelCPU.Temperature"\n'
                f'header = "{intel / "intel_cpu_temperature.h"}"\n'
                f'lib = "{intel / "intel_cpu_temperature.c"}"\n'
                'link = ["-lsensors"]\nsetup = "int intel_cpu_temperature_begin()"\n'
                'teardown = "void intel_cpu_temperature_end()"\n'
                'read = "double intel_cpu_get_temperature()"\nthermal = true\nunit = "C"\n')
            build = directory / "build"
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "-o", str(build)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.run(["nm", str(build / "greenmeasurer")], check=True,
                                     text=True, capture_output=True).stdout
            self.assertNotIn("rapl_msr_open", symbols)
            log = directory / "sensors.log"
            env = {**os.environ, "LD_PRELOAD": str(library), "SENSORS_LOG": str(log)}
            base = [str(build / "greenmeasurer"), "--aggregate", "-command", "true",
                    "-language", "Shell", "-program", "true", "-n_times", "1",
                    "-sleep-secs", "0", "-time_out_limit", "0"]
            result = subprocess.run([*base, "-output", str(directory / "none.csv"), "--no-metrics"],
                                    text=True, capture_output=True, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(log.exists())
            output = directory / "temperature.csv"
            result = subprocess.run([*base, "-output", str(output), "--IntelCPU.Temperature"],
                                    text=True, capture_output=True, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(log.read_text().split(), ["init", "read", "cleanup"])
            with output.open() as result_file:
                self.assertEqual(next(csv.DictReader(result_file))["IntelCPU.Temperature_C"], "NOT_SUPPORTED")
            result = subprocess.run([str(build / "greenmeasurer"), "--temperature-calibration",
                                     "-sleep-secs", "0", "--IntelCPU.Temperature",
                                     "-calib-file", f"IntelCPU.Temperature={directory / 'baseline.txt'}"],
                                    text=True, capture_output=True, env={**env, "FAIL_SENSORS": "1"})
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Failed to initialize the lm-sensors library", result.stderr)

    @unittest.skipUnless(
        Path("/usr/local/include/raplcap/raplcap.h").is_file()
        and Path("/usr/local/include/powercap/powercap.h").is_file()
        and Path("/usr/local/lib/libraplcap-powercap.a").is_file()
        and Path("/usr/local/lib/libpowercap.a").is_file(),
        "powercap development files are unavailable",
    )
    def test_linked_intel_powercap_is_inert_when_not_selected(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            intel = ROOT / "src/devices/linux/cpu/intel"
            config = directory / "powercap.toml"
            config.write_text(
                '[[aspect]]\nname = "IntelCPU.Powercap"\n'
                f'header = "{intel / "intel_cpu_powercap.h"}"\n'
                f'lib = "{intel / "intel_cpu_powercap.c"}"\n'
                'link = ["-L/usr/local/lib", "-lraplcap-powercap", "-lpowercap"]\n'
                'teardown = "void intel_cpu_powercap_end()"\n'
                'powercap_set = "int intel_cpu_set_powercap(int)"\n'
                'powercap_restore = "int intel_cpu_destroy_powercap()"\n'
                'powercap_calibration = "intel_cpu_calibrate_powercap"\n')
            build = directory / "build"
            result = subprocess.run([sys.executable, str(WEAVER), str(config), "-o", str(build)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            symbols = subprocess.run(["nm", str(build / "greenmeasurer")], check=True,
                                     text=True, capture_output=True).stdout
            self.assertIn("intel_cpu_set_powercap", symbols)
            result = subprocess.run(
                [str(build / "greenmeasurer"), "--aggregate", "-command", "true",
                 "-language", "Shell", "-program", "true", "-output", str(directory / "out.csv"),
                 "-n_times", "1", "-sleep-secs", "0", "-powercap", "-1", "-time_out_limit", "0",
                 "--no-metrics"], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("powercap", result.stderr.lower())

    def test_intel_powercap_dependencies_are_only_on_its_control_aspect(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        from weave import tomllib

        paths = list((ROOT / "examples").rglob("*.toml"))
        paths += list((ROOT / "configurations-generated").glob("*.toml"))
        for path in paths:
            with path.open("rb") as source:
                aspects = tomllib.load(source).get("aspect", [])
            for aspect in aspects:
                if aspect.get("name", "").startswith("IntelCPU."):
                    with self.subTest(path=path, aspect=aspect["name"]):
                        if aspect["name"] == "IntelCPU.Powercap":
                            self.assertTrue(aspect["lib"].endswith("libgm_intel_cpu_powercap.a"))
                            self.assertIn("-lraplcap-powercap", aspect["link"])
                            self.assertNotIn("read", aspect)
                        elif aspect["name"] == "IntelCPU.Temperature":
                            self.assertTrue(aspect["lib"].endswith("libgm_intel_cpu_temperature.a"))
                            self.assertEqual(aspect["link"], ["-lsensors"])
                            self.assertNotIn("-lpowercap", aspect["link"])
                        else:
                            self.assertTrue(aspect["lib"].endswith("libgm_intel_cpu.a"))
                            self.assertNotIn("-lraplcap-powercap", aspect.get("link", []))
                            self.assertNotIn("-lpowercap", aspect.get("link", []))
                            self.assertNotIn("-lsensors", aspect.get("link", []))


class WeaverScriptsTest(unittest.TestCase):
    def test_toy_example_powercap_sequence_and_failures(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            examples = root / "toy-examples"
            examples.mkdir()
            shutil.copy(ROOT / "toy-examples" / "run.sh", examples / "run.sh")
            (examples / "config.toml").write_text("")
            (root / "scripts").mkdir()
            (root / "src" / "devices").mkdir(parents=True)
            tools = root / "tools"
            tools.mkdir()
            make = tools / "make"
            make.write_text("#!/bin/sh\nexit 0\n")
            make.chmod(0o755)
            measure = root / "scripts" / "measure.sh"
            measure.write_text(
                '#!/bin/bash\n'
                'mkdir -p "$2/run-0001"\n'
                'cat > "$2/run-0001/greenmeasurer" <<\'EOF\'\n'
                '#!/bin/bash\n'
                'if [[ "$1" == --help ]]; then\n'
                '  printf "  --%s\\n" Time.WallClock IntelCPU.Temperature IntelCPU.Package NvidiaGPU.Temperature\n'
                '  if [[ "${MISSING_CAP:-}" != 1 ]]; then echo "  --IntelCPU.Powercap"; fi\n'
                '  exit 0\n'
                'fi\n'
                'printf "%s" "$1" >> "$RUN_LOG"\n'
                'shift\n'
                'printf "\\t%s" "$@" >> "$RUN_LOG"\n'
                'printf "\\n" >> "$RUN_LOG"\n'
                'if [[ "${FAIL_CALIBRATION:-}" == 1 && '
                '"$(tail -n 1 "$RUN_LOG")" == --powercap-calibration* ]]; then exit 2; fi\n'
                'EOF\n'
                'chmod +x "$2/run-0001/greenmeasurer"\n')
            log = root / "calls.log"
            env = {**os.environ, "PATH": f'{tools}{os.pathsep}{os.environ["PATH"]}',
                   "RUN_LOG": str(log)}
            for scenario in ("success", "failed", "missing"):
                with self.subTest(scenario=scenario):
                    log.unlink(missing_ok=True)
                    result = subprocess.run(
                        ["bash", str(examples / "run.sh")], cwd=root,
                        env={**env, "FAIL_CALIBRATION": str(int(scenario == "failed")),
                             "MISSING_CAP": str(int(scenario == "missing"))},
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0 if scenario == "success" else 1,
                                     result.stderr)
                    results = Path(result.stdout.strip().removeprefix("Results: "))
                    case = results / "06-powercap"
                    calls = [line.split("\t") for line in log.read_text().splitlines()]
                    for call in calls:
                        self.assertEqual(call[call.index("-sleep-secs") + 1], "10")
                        if "-temperature-control" in call:
                            self.assertEqual(call[call.index("-variance") + 1], "15")
                        if call[0] == "--over-time":
                            self.assertEqual(call[call.index("-interval-ms") + 1], "1")
                    caps = [call for call in calls if "--IntelCPU.Powercap" in call]
                    if scenario == "success":
                        self.assertEqual([call[0] for call in caps],
                                         ["--powercap-calibration", "--aggregate", "--over-time"])
                        self.assertEqual(caps[0][caps[0].index("-range") + 1], "5-7")
                        for call in caps[1:]:
                            self.assertEqual(call[call.index("-powercap") + 1], "-1,5")
                    elif scenario == "failed":
                        self.assertEqual([call[0] for call in caps], ["--powercap-calibration"])
                        self.assertIn("[SKIPPED]", (case / "aggregate.log").read_text())
                        self.assertIn("[SKIPPED]", (case / "over-time.log").read_text())
                    else:
                        self.assertFalse(caps)
                        self.assertIn("not incorporated", (case / "powercap-calibration.log").read_text())
                        for mode in ("aggregate", "over-time"):
                            self.assertNotIn("-powercap ", (case / f"{mode}.log").read_text())

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
                '[ -f "$2" ] || exit 1\n'
                'name=$(basename "$2")\n'
                'printf "generate:%s\\n" "$name" >> "$RUN_LOG"\n'
                'mkdir -p "$4"\n'
                'printf \'#!/bin/sh\\nprintf "run" >> "$RUN_LOG"\\n'
                'printf "\\\\t%%s" "$@" >> "$RUN_LOG"\\nprintf "\\\\n" >> "$RUN_LOG"\\n'
                '[ "${FAIL_ON:-}" != "$1" ]\\n\' > "$4/greenmeasurer"\n'
                'chmod +x "$4/greenmeasurer"\n')
            python.chmod(0o755)
            log = directory / "sequence.log"
            env = {**os.environ, "PATH": f'{tools}{os.pathsep}{os.environ["PATH"]}',
                   "RUN_LOG": str(log)}
            scripts = (ROOT / "configurations-generated" / "run.sh",
                       ROOT / "scripts" / "run_fibonacci_sequence.sh")
            for index, script in enumerate(scripts):
                with self.subTest(script=script):
                    log.unlink(missing_ok=True)
                    result = subprocess.run(
                        ["bash", os.path.relpath(script, directory), str(directory / f"build-{index}")],
                        cwd=directory,
                        env=env, text=True, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    events = log.read_text().splitlines()
                    self.assertEqual(events[0], "generate:config.toml")
                    self.assertEqual(len(events), 5)
                    runs = [event.split("\t")[1:] for event in events[1:]]
                    self.assertEqual(runs[0], [
                        "--temperature-calibration", "-sleep-secs", "60", "--IntelCPU.Temperature",
                        "-calib-file", "IntelCPU.Temperature=cores_temperatures.txt"])
                    metrics = ["--IntelCPU.Powercap", "--Time.WallClock", "--IntelCPU.Package",
                               "--IntelCPU.Core", "--IntelCPU.DRAM", "--IntelCPU.Temperature",
                               "--IntelGPU.Energy", "--NvidiaGPU.Energy", "--NvidiaGPU.Temperature"]
                    command = ["-command", "python3 Languages/Python/Fibonacci/fibonacci.py 42",
                               "-language", "Python", "-program", "fibonacci-42"]
                    thermal = ["-temperature-control", "IntelCPU.Temperature",
                               "-calib-file", "IntelCPU.Temperature=cores_temperatures.txt"]
                    self.assertEqual(runs[1], ["--powercap-calibration", *command,
                        "-output", "fibonacci-python-powercap-calibration.csv", "-range", "5-15",
                        "-n_times", "10", "-sleep-secs", "60", "-time_out_limit", "1",
                        *thermal, "-variance", "5", *metrics])
                    self.assertEqual(runs[2], ["--aggregate", *command,
                        "-output", "fibonacci-python-aggregate.csv", "-n_times", "10",
                        "-sleep-secs", "60", "-powercap", "-1,5", "-time_out_limit", "0",
                        *thermal, "-variance", "5", *metrics])
                    self.assertEqual(runs[3], ["--over-time", *command,
                        "-output", "fibonacci-python-overtime.csv", "-n_times", "10",
                        "-interval-ms", "1", "-sleep-secs", "60", "-powercap", "-1,5",
                        "-time_out_limit", "0", *thermal, "-variance", "15", *metrics])

                    log.unlink()
                    result = subprocess.run(
                        ["bash", os.path.relpath(script, directory), str(directory / f"failed-{index}")],
                        cwd=directory,
                        env={**env, "FAIL_ON": "--aggregate"}, text=True,
                        capture_output=True, timeout=30)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(log.read_text().splitlines(), events[:4])

    def test_batch_generation_and_execution(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "metric.h").write_text("int metric(void);\n")
            (directory / "metric.c").write_text("int metric(void) { return 1; }\n")
            configs = []
            for mode in ("aggregate", "over-time"):
                config = directory / f"{mode}.toml"
                config.write_text(
                    '[[aspect]]\nname = "Toy.Value"\nheader = "metric.h"\nlib = "metric.c"\n'
                    'read = "int metric()"\n')
                configs.append(config)
            script = ROOT / "scripts" / "measure.sh"
            batch = directory / "batch"
            result = subprocess.run(
                ["bash", str(script), "generate", str(batch), *map(str, configs)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((directory / "aggregate.csv").exists())
            output = directory / "batch.csv"
            arguments = ["--", "--aggregate", "-command", "true", "-language", "Shell",
                         "-program", "true", "-output", str(output), "-n_times", "1",
                         "-sleep-secs", "0", "-powercap", "-1", "-time_out_limit", "0",
                         "--Toy.Value"]
            result = subprocess.run(["bash", str(script), "run", str(batch), *arguments],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open() as source:
                self.assertEqual(len(list(csv.DictReader(source))), 2)
            (batch / "run-0002" / "greenmeasurer").unlink()
            result = subprocess.run(["bash", str(script), "run", str(batch), *arguments],
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
            result = subprocess.run(["bash", str(script), "run", str(incomplete), *arguments],
                                    text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not completely generated", result.stderr)

    def test_calibration_wrapper_rejects_missing_hook(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            config = directory / "aggregate.toml"
            config.write_text("")
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_powercap_calibration.sh"),
                 str(config), str(directory / "build")],
                text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must declare powercap_calibration", result.stderr)
            self.assertFalse((directory / "build").exists())

    def test_calibration_wrapper_generates_configured_executable(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            for filename in ("toy.c", "toy.h"):
                shutil.copy(FIXTURES / filename, directory / filename)
            config = directory / "calibration.toml"
            config.write_text(
                (FIXTURES / "toy_hooks.toml").read_text()
                .replace('lib = "libtoy.a"', 'lib = "toy.c"'))
            build = directory / "build"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_powercap_calibration.sh"),
                 str(config), str(build)], text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(build / "greenmeasurer"), "--powercap-calibration",
                                     "-range", "2-2", "-command", "true", "-language", "Shell",
                                     "-program", "true", "-output", "hooks.csv", "-n_times", "1",
                                     "-sleep-secs", "0", "-time_out_limit", "0", "--Cpu.Package",
                                     "--Cpu.Powercap"],
                                    cwd=directory,
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((directory / "hooks.csv").is_file())

    def test_calibration_wrapper_fails_if_control_cannot_be_built(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "hooks.h").write_text("int calibrate(void);\n")
            config = directory / "control.toml"
            config.write_text(
                '[[aspect]]\nname = "Control"\nheader = "hooks.h"\nlib = "missing.a"\n'
                'powercap_calibration = "calibrate"\n')
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_powercap_calibration.sh"),
                 str(config), str(directory / "build")],
                text=True, capture_output=True, timeout=120)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Skipping aspect 'Control'", result.stderr)
            self.assertIn("No declared powercap calibration aspect could be built", result.stderr)

    def test_run_all_configurations_uses_new_format(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            config = directory / "aggregate.toml"
            config.write_text("")
            results = directory / "results"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_all_configurations.sh"),
                 "-o", str(results), str(config)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("PASS: 1  WARN: 0  FAIL: 0", result.stdout)
            self.assertFalse(list(results.rglob("measurements.csv")))

    def test_run_all_warns_when_no_aspects_are_available(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "metric.h").write_text("int setup(void);\nint metric(void);\n")
            (directory / "metric.c").write_text(
                "int setup(void) { return 1; }\nint metric(void) { return 1; }\n")
            config = directory / "unavailable.toml"
            config.write_text(
                '[[aspect]]\nname = "Toy.Value"\nheader = "metric.h"\nlib = "metric.c"\n'
                'setup = "int setup()"\nread = "int metric()"\n')
            results = directory / "results"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_all_configurations.sh"),
                 "-o", str(results), str(config)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("PASS: 0  WARN: 1  FAIL: 0", result.stdout)
            self.assertFalse(list(results.rglob("results.csv")))

    def test_run_all_selects_only_aspects_that_were_built(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "metric.h").write_text("int metric(void);\n")
            (directory / "metric.c").write_text("int metric(void) { return 1; }\n")
            config = directory / "mixed.toml"
            config.write_text(
                '[[aspect]]\nname = "Missing"\nheader = "missing.h"\nread = "int metric()"\n'
                '[[aspect]]\nname = "Present"\nheader = "metric.h"\nlib = "metric.c"\n'
                'read = "int metric()"\n')
            results = directory / "results"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_all_configurations.sh"),
                 "-o", str(results), str(config)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("PASS: 0  WARN: 1  FAIL: 0", result.stdout)
            self.assertIn("Skipping aspect 'Missing'", next(results.rglob("generate.log")).read_text())
            with next(results.rglob("results.csv")).open() as source:
                row = next(csv.DictReader(source))
            self.assertEqual(row["Present"], "1")
            self.assertNotIn("Missing", row)

    def test_run_all_selects_only_one_powercap_control(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "hooks.h").write_text(
                "int metric(void);\nint set(int);\nint restore(void);\n")
            (directory / "hooks.c").write_text(
                "int metric(void) { return 1; }\nint set(int watts) { return 0; }\n"
                "int restore(void) { return 0; }\n")
            config = directory / "controls.toml"
            config.write_text(
                '[[aspect]]\nname = "Value"\nheader = "hooks.h"\nlib = "hooks.c"\n'
                'read = "int metric()"\n' +
                ''.join(f'[[aspect]]\nname = "{name}"\nheader = "hooks.h"\nlib = "hooks.c"\n'
                        'powercap_set = "int set(int)"\npowercap_restore = "int restore()"\n'
                        for name in ("First", "Second")))
            results = directory / "results"
            result = subprocess.run(
                ["bash", str(ROOT / "scripts" / "run_all_configurations.sh"),
                 "-o", str(results), str(config)],
                text=True, capture_output=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("PASS: 0  WARN: 1  FAIL: 0", result.stdout)
            self.assertIn("Skipping additional powercap aspect Second",
                          next(results.rglob("generate.log")).read_text())

    def test_run_all_reports_warnings_and_continues_after_failure(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "metric.h").write_text("int metric(void);\n")
            (directory / "metric.c").write_text("int metric(void) { return -1; }\n")
            (directory / "a-invalid.toml").write_text('[measurement]\nmode = "invalid"\n')
            (directory / "b-warning.toml").write_text(
                '[[aspect]]\nname = "Toy.Value"\nheader = "metric.h"\nlib = "metric.c"\n'
                'read = "int metric()"\n')
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
            self.assertIn("NOT_SUPPORTED", next(results.rglob("results.csv")).read_text())

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
