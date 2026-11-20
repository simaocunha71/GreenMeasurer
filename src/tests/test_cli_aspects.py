import csv
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "src/tests/fixtures/weaver"


class AspectCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        config = cls.directory / "aspects.toml"
        common = (f'header = "{FIXTURES / "toy.h"}"\n'
                  f'lib = "{FIXTURES / "toy.c"}"\n'
                  'setup = "int toy_setup()"\n'
                  'teardown = "void toy_teardown()"\n')
        config.write_text(
            '[[aspect]]\nname = "Time.WallClock"\n'
            f'header = "{ROOT / "src/aspects/wall_clock_time.h"}"\n'
            f'lib = "{ROOT / "src/aspects/wall_clock_time.c"}"\n'
            'measurement_start = "int gm_wall_clock_time_start()"\n'
            'measurement_stop = "int gm_wall_clock_time_stop()"\n'
            'read = "double gm_wall_clock_time_read()"\nkind = "duration"\nunit = "ms"\n'
            '[[aspect]]\nname = "Energy.Package"\n' + common +
            'read = "unsigned long toy_energy()"\nkind = "counter"\nunit = "uJ"\n'
            '[[aspect]]\nname = "CPU.Powercap"\n' + common +
            'powercap_set = "int toy_cap_set(int)"\n'
            'powercap_restore = "int toy_cap_restore()"\n'
            'powercap_calibration = "toy_calibrate"\n'
            '[[aspect]]\nname = "CPU.OtherPowercap"\n' + common +
            'powercap_set = "int toy_cap_set(int)"\n'
            'powercap_restore = "int toy_cap_restore()"\n'
            '[[aspect]]\nname = "CPU.Temperature"\n' + common +
            'read = "double toy_thermal()"\nunit = "C"\nthermal = true\n'
            '[[aspect]]\nname = "GPU.Temperature"\n' + common +
            'read = "float toy_temperature()"\nunit = "C"\nthermal = true\n')
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts/weave.py"), str(config),
             "-o", str(cls.directory / "build")], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.binary = cls.directory / "build/greenmeasurer"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_binary(self, *args, **env):
        return subprocess.run([str(self.binary), *args], capture_output=True, text=True,
                              env={**os.environ, **env}, timeout=30)

    def measure_args(self, output):
        return ["--aggregate", "-command", "true", "-language", "Shell", "-program", "true",
                "-output", str(output), "-n_times", "1", "-sleep-secs", "0",
                "-powercap", "-1", "-time_out_limit", "0"]

    def test_selects_one_aspect_without_reading_temperature(self):
        output = self.directory / "energy.csv"
        log = self.directory / "events.log"
        result = self.run_binary(*self.measure_args(output), "--Energy.Package",
                                 TOY_FAIL_TEMPERATURE="1", TOY_LOG=str(log))
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open() as source:
            rows = list(csv.DictReader(source))
        self.assertEqual(list(rows[0]), ["Language", "Program", "Powercap",
                                        "Energy.Package_uJ"])
        self.assertEqual(rows[0]["Energy.Package_uJ"], "10")
        self.assertEqual(log.read_text().split(), ["setup", "teardown"])

    def test_powercap_control_is_independent_of_metric_selection(self):
        for mode in ("--aggregate", "--over-time"):
            with self.subTest(mode=mode):
                output = self.directory / f"powercap-only-{mode}.csv"
                output.unlink(missing_ok=True)
                log = self.directory / f"powercap-only-{mode}.log"
                log.unlink(missing_ok=True)
                args = self.measure_args(output)
                args[0] = mode
                args[args.index("-powercap") + 1] = "-1,5"
                if mode == "--over-time":
                    args += ["-interval-ms", "5"]
                result = self.run_binary(*args, "--no-metrics", "--CPU.Powercap",
                                         TOY_LOG=str(log))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(log.read_text().split(),
                                 ["setup", "set:-1", "restore", "set:5", "restore", "teardown"])
                with output.open() as source:
                    rows = list(csv.DictReader(source))
                self.assertEqual(list(rows[0]), ["Language", "Program", "Powercap"] +
                                 (["Timestamp_ms"] if mode == "--over-time" else []))
                self.assertEqual({row["Powercap"] for row in rows}, {"-1", "5"})

    def test_measurement_without_powercap_option_does_not_activate_control(self):
        for mode in ("--aggregate", "--over-time"):
            for selection in ("--Energy.Package", "--no-metrics"):
                with self.subTest(mode=mode, selection=selection):
                    output = self.directory / f"no-powercap-{mode}-{selection}.csv"
                    log = self.directory / f"no-powercap-{mode}-{selection}.log"
                    args = self.measure_args(output)
                    args[0] = mode
                    index = args.index("-powercap")
                    del args[index:index + 2]
                    if mode == "--over-time":
                        args += ["-interval-ms", "5"]
                    result = self.run_binary(*args, selection, TOY_LOG=str(log))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if selection == "--no-metrics":
                        self.assertFalse(output.exists())
                    else:
                        with output.open() as source:
                            rows = list(csv.DictReader(source))
                        self.assertTrue(rows)
                        self.assertTrue(all(row["Powercap"] == "-1" for row in rows))
                    events = log.read_text().split() if log.exists() else []
                    self.assertEqual(events, ["setup", "teardown"] if selection == "--Energy.Package" else [])

    def test_selected_powercap_requires_explicit_limit(self):
        output = self.directory / "powercap-missing-limit.csv"
        args = self.measure_args(output)
        index = args.index("-powercap")
        del args[index:index + 2]
        result = self.run_binary(*args, "--no-metrics", "--CPU.Powercap")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires -powercap", result.stderr)
        self.assertFalse(output.exists())

    def test_powercap_requires_explicit_unambiguous_selection(self):
        output = self.directory / "powercap-selection.csv"
        output.unlink(missing_ok=True)
        args = self.measure_args(output)
        args[args.index("-powercap") + 1] = "5"
        result = self.run_binary(*args, "--Energy.Package")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Select a powercap control aspect", result.stderr)
        self.assertFalse(output.exists())
        result = self.run_binary(*args, "--no-metrics", "--CPU.Powercap", "--CPU.OtherPowercap")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("at most one powercap control aspect", result.stderr)
        result = self.run_binary(*args, "--no-metrics", "--CPU.Powercap", TOY_FAIL_SETUP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Setup of aspect 'CPU.Powercap' failed", result.stderr)
        self.assertIn("Powercap limits will not be applied", result.stderr)
        self.assertFalse(output.exists())

        result = self.run_binary("--powercap-calibration", "-range", "2-2", "-command", "true",
                                 "-language", "Shell", "-program", "true", "-output", str(output),
                                 "-n_times", "1", "-sleep-secs", "0", "-time_out_limit", "0",
                                 "--Energy.Package")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Select a powercap control aspect", result.stderr)
        self.assertFalse(output.exists())

    def test_powercap_hooks_restore_and_report_failures(self):
        output = self.directory / "powercap-failure.csv"
        log = self.directory / "powercap-failure.log"
        for variable in ("TOY_FAIL_CAP_SET", "TOY_FAIL_CAP_RESTORE"):
            with self.subTest(variable=variable):
                output.unlink(missing_ok=True)
                log.unlink(missing_ok=True)
                args = self.measure_args(output)
                args[args.index("-powercap") + 1] = "5"
                result = self.run_binary(*args, "--Energy.Package", "--CPU.Powercap",
                                         TOY_LOG=str(log), **{variable: "1"})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Failed to " + ("apply" if variable == "TOY_FAIL_CAP_SET"
                                                else "restore"), result.stderr)
                self.assertEqual(log.read_text().split(), ["setup", "set:5", "restore", "teardown"])

    def test_powercap_only_over_time_does_not_start_a_sampling_thread(self):
        source = self.directory / "deny-powercap-threads.c"
        library = self.directory / "deny-powercap-threads.so"
        source.write_text(
            "#include <pthread.h>\n#include <errno.h>\n"
            "int pthread_create(pthread_t *t, const pthread_attr_t *a, "
            "void *(*start)(void *), void *arg) { return EAGAIN; }\n")
        subprocess.run(["gcc", "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
        output = self.directory / "powercap-no-thread.csv"
        args = self.measure_args(output)
        args[0] = "--over-time"
        result = self.run_binary(*args, "--no-metrics", "--CPU.Powercap",
                                 "-interval-ms", "5", LD_PRELOAD=str(library))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_execution_time_is_selected_and_uses_workload_boundaries(self):
        for mode in ("--aggregate", "--over-time"):
            with self.subTest(mode=mode):
                output = self.directory / f"elapsed-{mode}.csv"
                args = self.measure_args(output)
                args[0] = mode
                args[args.index("-command") + 1] = "sleep 0.1"
                if mode == "--over-time":
                    args += ["-interval-ms", "20"]
                result = self.run_binary(*args, "--Time.WallClock")
                self.assertEqual(result.returncode, 0, result.stderr)
                with output.open() as source:
                    rows = list(csv.DictReader(source))
                self.assertTrue(rows)
                elapsed = float(rows[-1]["Time.WallClock_ms"])
                self.assertGreaterEqual(elapsed, 80)
                self.assertLess(elapsed, 1000)
                self.assertNotIn("Time_ms", rows[-1])
                if mode == "--over-time":
                    self.assertLess(abs(float(rows[-1]["Timestamp_ms"]) - elapsed), 30)
                    values = [float(row["Time.WallClock_ms"]) for row in rows
                              if row["Time.WallClock_ms"] != "NOT_SUPPORTED"]
                    self.assertEqual(values, sorted(values))

        output = self.directory / "no-time.csv"
        result = self.run_binary(*self.measure_args(output), "--no-metrics")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(output.exists())

    def test_custom_duration_callbacks_and_failures(self):
        header = self.directory / "custom-time.h"
        source = self.directory / "custom-time.c"
        config = self.directory / "custom-time.toml"
        header.write_text("int begin(void);\nint end(void);\ndouble elapsed(void);\n")
        source.write_text(
            '#include <stdlib.h>\nstatic int active;\n'
            'int begin(void) { if (getenv("FAIL_BEGIN")) return -1; active = 1; return 0; }\n'
            'int end(void) { active = 0; return getenv("FAIL_END") ? -1 : 0; }\n'
            'double elapsed(void) { return active ? 10 : 20; }\n')
        config.write_text(
            '[[aspect]]\nname="Time.Custom"\nheader="custom-time.h"\nlib="custom-time.c"\n'
            'measurement_start="int begin()"\nmeasurement_stop="int end()"\n'
            'read="double elapsed()"\nkind="duration"\nunit="ms"\n')
        build = self.directory / "custom-time-build"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/weave.py"),
                                 str(config), "-o", str(build)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.directory / "custom-time.csv"
        args = self.measure_args(output)
        args[args.index("-command") + 1] = "sleep 0.03"
        for mode in ("--aggregate", "--over-time"):
            output.unlink(missing_ok=True)
            args[0] = mode
            result = subprocess.run(
                [str(build / "greenmeasurer"), *args,
                 *(["-interval-ms", "5"] if mode == "--over-time" else []), "--Time.Custom"],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open() as result_file:
                rows = list(csv.DictReader(result_file))
            self.assertEqual(rows[-1]["Time.Custom_ms"], "20")
        for variable in ("FAIL_BEGIN", "FAIL_END"):
            output.unlink(missing_ok=True)
            args[0] = "--aggregate"
            result = subprocess.run([str(build / "greenmeasurer"), *args, "--Time.Custom"],
                                    capture_output=True, text=True, timeout=30,
                                    env={**os.environ, variable: "1"})
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Failed to", result.stderr)
            with output.open() as result_file:
                self.assertEqual(list(csv.DictReader(result_file)), [])

    def test_duration_excludes_counter_reading_overhead(self):
        header = self.directory / "slow.h"
        source = self.directory / "slow.c"
        config = self.directory / "slow.toml"
        header.write_text("int slow_counter(void);\n")
        source.write_text("#include <unistd.h>\nint slow_counter(void) { usleep(200000); return 1; }\n")
        config.write_text(
            '[[aspect]]\nname="Time.WallClock"\n'
            f'header="{ROOT / "src/aspects/wall_clock_time.h"}"\n'
            f'lib="{ROOT / "src/aspects/wall_clock_time.c"}"\n'
            'measurement_start="int gm_wall_clock_time_start()"\n'
            'measurement_stop="int gm_wall_clock_time_stop()"\n'
            'read="double gm_wall_clock_time_read()"\nkind="duration"\nunit="ms"\n'
            '[[aspect]]\nname="Slow.Counter"\nheader="slow.h"\nlib="slow.c"\n'
            'read="int slow_counter()"\nkind="counter"\n')
        build = self.directory / "slow-build"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/weave.py"),
                                 str(config), "-o", str(build)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.directory / "slow.csv"
        args = self.measure_args(output)
        args[args.index("-command") + 1] = "sleep 0.1"
        started = time.monotonic()
        result = subprocess.run([str(build / "greenmeasurer"), *args,
                                 "--Time.WallClock", "--Slow.Counter"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        total_ms = (time.monotonic() - started) * 1000
        with output.open() as result_file:
            elapsed = float(next(csv.DictReader(result_file))["Time.WallClock_ms"])
        self.assertGreaterEqual(elapsed, 80)
        self.assertGreater(total_ms - elapsed, 350)

    def test_informational_temperature_does_not_control_or_require_calibration(self):
        baseline = self.directory / "control-cpu.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        for unavailable in (False, True):
            output = self.directory / f"informational-{unavailable}.csv"
            args = self.measure_args(output)
            args[args.index("-n_times") + 1] = "2"
            args[args.index("-sleep-secs") + 1] = "1"
            started = time.monotonic()
            result = self.run_binary(*args, "--CPU.Temperature", "--GPU.Temperature",
                                     "-temperature-control", "CPU.Temperature", "-variance", "5",
                                     "-calib-file", f"CPU.Temperature={baseline}",
                                     **({"TOY_FAIL_TEMPERATURE": "1"} if unavailable else {}))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreaterEqual(time.monotonic() - started, 0.8)
            self.assertNotIn("thermal control", result.stderr)
            with output.open() as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(len(rows), 2)
            self.assertTrue(all(row["CPU.Temperature_C"] == "40" for row in rows))
            self.assertEqual(rows[0]["GPU.Temperature_C"] == "NOT_SUPPORTED", unavailable)

        result = self.run_binary(*self.measure_args(self.directory / "information-only.csv"),
                                 "--GPU.Temperature", TOY_FAIL_TEMPERATURE="1")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_thermal_control_requires_a_temperature_and_its_file(self):
        base = self.measure_args(self.directory / "invalid-control.csv")
        cases = [
            (["--CPU.Temperature", "-temperature-control", "missing"], "unique temperature"),
            (["--Energy.Package", "-temperature-control", "Energy.Package"], "unique temperature"),
            (["--CPU.Temperature", "-temperature-control", "CPU.Temperature"], "-calib-file"),
            (["--CPU.Temperature", "-temperature-control", "CPU.Temperature",
              "-temperature-control", "CPU.Temperature"], "unique temperature"),
            (["--CPU.Temperature", "-calib-file", "CPU.Temperature=unused.txt"], "uncontrolled"),
        ]
        for arguments, message in cases:
            with self.subTest(arguments=arguments):
                result = self.run_binary(*base, *arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_temperature_control_option_is_exposed_in_help(self):
        help_result = self.run_binary("--help")
        self.assertIn("-temperature-control", help_result.stdout)
        result = self.run_binary(*self.measure_args(self.directory / "old-control.csv"),
                                 "--CPU.Temperature", "-thermal-control", "CPU.Temperature")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown", result.stderr)

    def test_aggregate_is_the_measurement_mode(self):
        help_result = self.run_binary("--help")
        self.assertIn("--aggregate", help_result.stdout)
        self.assertNotIn("--standard", help_result.stdout)
        result = self.run_binary("--standard")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown", result.stderr)

    def test_control_can_use_a_temperature_without_collecting_it(self):
        baseline = self.directory / "control-only-cpu.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        output = self.directory / "control-only.csv"
        result = self.run_binary(*self.measure_args(output), "--GPU.Temperature",
                                 "-temperature-control", "CPU.Temperature",
                                 "-calib-file", f"CPU.Temperature={baseline}", "-variance", "0",
                                 TOY_CPU_COOLS="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Waiting for temperature CPU.Temperature", result.stdout)
        with output.open() as source:
            row = next(csv.DictReader(source))
        self.assertIn("GPU.Temperature_C", row)
        self.assertNotIn("CPU.Temperature_C", row)

    def test_temperature_requires_a_path_for_each_selected_aspect(self):
        cpu = self.directory / "cpu.txt"
        gpu = self.directory / "gpu.txt"
        args = ["--temperature-calibration", "-sleep-secs", "0",
                "--CPU.Temperature", "--GPU.Temperature",
                "-calib-file", f"CPU.Temperature={cpu}"]
        missing = self.run_binary(*args)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("GPU.Temperature", missing.stderr)
        result = self.run_binary(*args, "-calib-file", f"GPU.Temperature={gpu}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("40.0", cpu.read_text())
        self.assertIn("41.5", gpu.read_text())

    def test_selected_temperatures_use_separate_baselines_and_shared_setup(self):
        cpu = self.directory / "measured-cpu.txt"
        gpu = self.directory / "measured-gpu.txt"
        cpu.write_text("Average temperature: 40.0 C\n")
        gpu.write_text("Average temperature: 41.0 C\n")
        log = self.directory / "shared.log"
        result = self.run_binary(*self.measure_args(self.directory / "temperatures.csv"),
                                 "--CPU.Temperature", "--GPU.Temperature",
                                 "-temperature-control", "CPU.Temperature", "-temperature-control", "GPU.Temperature",
                                 "-calib-file", f"CPU.Temperature={cpu}",
                                 "-calib-file", f"GPU.Temperature={gpu}", "-variance", "1",
                                 TOY_LOG=str(log))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log.read_text().split(), ["setup", "teardown"])
        duplicate = self.run_binary(*self.measure_args(self.directory / "duplicate.csv"),
                                    "--CPU.Temperature", "--GPU.Temperature",
                                    "-temperature-control", "CPU.Temperature", "-temperature-control", "GPU.Temperature",
                                    "-calib-file", f"CPU.Temperature={cpu}",
                                    "-calib-file", f"GPU.Temperature={cpu}", "-variance", "1")
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertIn("separate calibration files", duplicate.stderr)

    def test_rejects_temperature_options_for_unselected_aspects(self):
        result = self.run_binary(*self.measure_args(self.directory / "unselected.csv"),
                                 "--Energy.Package", "-calib-file", "CPU.Temperature=unused.txt")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unselected or uncontrolled aspect", result.stderr)
        self.assertFalse((self.directory / "unselected.csv").exists())

    def test_invalid_baseline_fails_without_replacing_it(self):
        baseline = self.directory / "invalid-temperature.txt"
        baseline.write_text("invalid\n")
        result = self.run_binary(*self.measure_args(self.directory / "invalid-baseline.csv"),
                                 "--CPU.Temperature", "-temperature-control", "CPU.Temperature", "-variance", "5",
                                 "-calib-file", f"CPU.Temperature={baseline}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("calibration", result.stderr)
        self.assertEqual(baseline.read_text(), "invalid\n")

    def test_unavailable_temperature_skips_its_control_and_repetition_pauses(self):
        cpu = self.directory / "available-cpu.txt"
        cpu.write_text("Average temperature: 40.0 C\n")
        gpu = self.directory / "unavailable-gpu.txt"
        for mode in ("--aggregate", "--over-time", "--powercap-calibration"):
            for variable in ("TOY_FAIL_TEMPERATURE", "TOY_FAIL_TEMPERATURE_AFTER_FIRST"):
                with self.subTest(mode=mode, variable=variable):
                    output = self.directory / f"unavailable-{mode}-{variable}.csv"
                    args = self.measure_args(output)
                    args[0] = mode
                    args[args.index("-n_times") + 1] = "2"
                    args[args.index("-sleep-secs") + 1] = "4"
                    if mode == "--powercap-calibration":
                        index = args.index("-powercap")
                        del args[index:index + 2]
                        args += ["-range", "2-2", "--CPU.Powercap"]
                    elif mode == "--over-time":
                        args += ["-interval-ms", "5"]
                    if variable == "TOY_FAIL_TEMPERATURE_AFTER_FIRST":
                        gpu.write_text("Average temperature: 40.5 C\n")
                    else:
                        gpu.unlink(missing_ok=True)
                    started = time.monotonic()
                    result = self.run_binary(*args, "--CPU.Temperature", "--GPU.Temperature",
                                             "-temperature-control", "CPU.Temperature", "-temperature-control", "GPU.Temperature",
                                             "-calib-file", f"CPU.Temperature={cpu}",
                                             "-calib-file", f"GPU.Temperature={gpu}",
                                             "-variance", "5", **{variable: "1"})
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertLess(time.monotonic() - started, 3)
                    self.assertIn("thermal control", result.stderr)
                    if variable == "TOY_FAIL_TEMPERATURE":
                        self.assertFalse(gpu.exists())
                    with output.open() as source:
                        rows = list(csv.DictReader(source))
                    self.assertTrue(rows)
                    self.assertTrue(all(row["GPU.Temperature_C"] == "NOT_SUPPORTED" for row in rows))
                    self.assertTrue(all(row["CPU.Temperature_C"] == "40" for row in rows))

    def test_unavailable_temperature_cannot_be_calibrated(self):
        baseline = self.directory / "unavailable-calibration.txt"
        result = self.run_binary("--temperature-calibration", "-sleep-secs", "4",
                                 "--GPU.Temperature", "-calib-file",
                                 f"GPU.Temperature={baseline}", TOY_FAIL_TEMPERATURE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unavailable", result.stderr)
        self.assertFalse(baseline.exists())

    def test_unavailable_temperature_skips_automatic_calibration_wait(self):
        cpu = self.directory / "automatic-cpu.txt"
        gpu = self.directory / "automatic-gpu.txt"
        args = self.measure_args(self.directory / "automatic-calibration.csv")
        args[args.index("-sleep-secs") + 1] = "4"
        started = time.monotonic()
        result = self.run_binary(*args, "--CPU.Temperature", "--GPU.Temperature",
                                 "-temperature-control", "CPU.Temperature", "-temperature-control", "GPU.Temperature",
                                 "-calib-file", f"CPU.Temperature={cpu}",
                                 "-calib-file", f"GPU.Temperature={gpu}", "-variance", "5",
                                 TOY_FAIL_TEMPERATURE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(time.monotonic() - started, 3)
        self.assertTrue(cpu.exists())
        self.assertFalse(gpu.exists())

    def test_temperature_setup_failure_does_not_stop_measurement(self):
        baseline = self.directory / "setup-failure-baseline.txt"
        output = self.directory / "setup-failure.csv"
        result = self.run_binary(*self.measure_args(output), "--GPU.Temperature",
                                 "-temperature-control", "GPU.Temperature",
                                 "-calib-file", f"GPU.Temperature={baseline}", "-variance", "5",
                                 TOY_FAIL_SETUP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("thermal control", result.stderr)
        self.assertFalse(baseline.exists())
        self.assertFalse(output.exists())

    def test_temperature_can_become_unavailable_during_automatic_calibration(self):
        baseline = self.directory / "lost-during-calibration.txt"
        result = self.run_binary(*self.measure_args(self.directory / "lost-during-calibration.csv"),
                                 "--GPU.Temperature", "-temperature-control", "GPU.Temperature", "-calib-file",
                                 f"GPU.Temperature={baseline}", "-variance", "5",
                                 TOY_FAIL_TEMPERATURE_AFTER_FIRST="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("thermal control", result.stderr)
        self.assertFalse(baseline.exists())

    def test_baselines_and_output_cannot_alias(self):
        baseline = self.directory / "protected-baseline.txt"
        baseline.write_text("Average temperature: 40.0 C\n")
        alias = self.directory / "baseline-alias.txt"
        os.link(baseline, alias)
        result = self.run_binary(*self.measure_args(alias), "--CPU.Temperature", "-variance", "5",
                                 "-temperature-control", "CPU.Temperature",
                                 "-calib-file", f"CPU.Temperature={baseline}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be separate", result.stderr)
        self.assertEqual(baseline.read_text(), "Average temperature: 40.0 C\n")
        result = self.run_binary("--temperature-calibration", "-sleep-secs", "0",
                                 "--CPU.Temperature", "--GPU.Temperature",
                                 "-calib-file", f"CPU.Temperature={baseline}",
                                 "-calib-file", f"GPU.Temperature={alias}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("separate calibration files", result.stderr)
        self.assertEqual(baseline.read_text(), "Average temperature: 40.0 C\n")

    def test_rejects_csv_with_different_selected_aspects(self):
        output = self.directory / "schema.csv"
        first = self.run_binary(*self.measure_args(output), "--Energy.Package")
        self.assertEqual(first.returncode, 0, first.stderr)
        before = output.read_text()
        second = self.run_binary(*self.measure_args(output), "--Time.WallClock")
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("CSV header does not match", second.stderr)
        self.assertEqual(output.read_text(), before)

    def test_over_time_and_powercap_calibration_are_selected_at_runtime(self):
        output = self.directory / "over-time.csv"
        overtime = self.measure_args(output)
        overtime[0] = "--over-time"
        result = self.run_binary(*overtime, "-interval-ms", "5", "--Energy.Package")
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open() as source:
            self.assertIn("Timestamp_ms", next(csv.reader(source)))
        sweep = self.directory / "sweep.csv"
        result = self.run_binary("--powercap-calibration", "-range", "2-2", "-command", "true",
                                 "-language", "Shell", "-program", "true", "-output", str(sweep),
                                 "-n_times", "1", "-sleep-secs", "0", "-time_out_limit", "0",
                                 "--Energy.Package", "--CPU.Powercap")
        self.assertEqual(result.returncode, 0, result.stderr)
        with sweep.open() as source:
            self.assertEqual([row["Powercap"] for row in csv.DictReader(source)], ["-1", "2"])

    def test_powercap_summary_uses_selected_duration_aspect(self):
        output = self.directory / "duration-sweep.csv"
        result = self.run_binary("--powercap-calibration", "-range", "2-2", "-command", "true",
                                 "-language", "Shell", "-program", "true", "-output", str(output),
                                 "-n_times", "1", "-sleep-secs", "0", "-time_out_limit", "0",
                                 "--Energy.Package", "--Time.WallClock", "--CPU.Powercap")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Processing calibration CSV", result.stdout)
        if importlib.util.find_spec("pandas"):
            self.assertNotIn("Calibration post-processing script exited with error", result.stderr)
            self.assertIn("Powercap GPS-UP Energy Efficiency Report", result.stdout)
        with output.open() as source:
            rows = list(csv.DictReader(source))
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(float(row["Time.WallClock_ms"]) > 0 for row in rows))

    def test_aspects_from_same_header_do_not_read_concurrently(self):
        header = self.directory / "shared.h"
        source = self.directory / "shared.c"
        config = self.directory / "shared.toml"
        header.write_text("int first(void);\nint second(void);\n")
        source.write_text(
            "#include <unistd.h>\nstatic int active, violation;\n"
            "static int read_value(void) {\n"
            "  int busy = __sync_lock_test_and_set(&active, 1);\n"
            "  if (busy) __atomic_store_n(&violation, 1, __ATOMIC_RELAXED);\n"
            "  usleep(3000);\n"
            "  if (!busy) __sync_lock_release(&active);\n"
            "  return __atomic_load_n(&violation, __ATOMIC_RELAXED) ? -1 : 1;\n"
            "}\nint first(void) { return read_value(); }\n"
            "int second(void) { return read_value(); }\n")
        config.write_text(
            '[[aspect]]\nname="Shared.First"\nheader="shared.h"\nlib="shared.c"\n'
            'read="int first()"\n'
            '[[aspect]]\nname="Shared.Second"\nheader="shared.h"\nlib="shared.c"\n'
            'read="int second()"\n')
        build = self.directory / "shared-build"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/weave.py"),
                                 str(config), "-o", str(build)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.directory / "shared.csv"
        args = self.measure_args(output)
        args[0] = "--over-time"
        args[2] = "sleep 0.1"
        result = subprocess.run([str(build / "greenmeasurer"), *args, "-interval-ms", "1",
                                 "--Shared.First", "--Shared.Second"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open() as source_file:
            rows = list(csv.DictReader(source_file))
        self.assertTrue(rows)
        self.assertEqual(rows[-1]["Shared.First"], "1")
        self.assertEqual(rows[-1]["Shared.Second"], "1")

    def test_explicit_mode_and_options_are_required(self):
        result = self.run_binary("--Energy.Package")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mode", result.stderr)
        result = self.run_binary("--aggregate", "--Energy.Package")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required execution options", result.stderr)
        baseline = self.directory / "variance.txt"
        result = self.run_binary(*self.measure_args(self.directory / "variance.csv"),
                                 "--CPU.Temperature", "-temperature-control", "CPU.Temperature", "-calib-file",
                                 f"CPU.Temperature={baseline}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("-variance", result.stderr)
        self.assertFalse(baseline.exists())

    def test_invalid_powercap_ranges_are_rejected(self):
        for value in ("0-3", "5-4", "1-128", "99999999999999999999-3"):
            result = self.run_binary("--powercap-calibration", "-range", value,
                                     "-command", "true", "-language", "Shell", "-program", "true",
                                     "-output", str(self.directory / "bad-range.csv"), "-n_times", "1",
                                     "-sleep-secs", "0", "-time_out_limit", "0", "--Energy.Package")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("-range", result.stderr)


if __name__ == "__main__":
    unittest.main()
