import csv
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


class BackendRegistryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(SRC), "all"], check=True,
                       capture_output=True, text=True)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.backends = self.directory / "backends"
        self.backends.mkdir()
        self.env = os.environ.copy()
        self.env["GREENMEASURER_BACKEND_DIR"] = str(self.backends)
        self.calibration = self.directory / "calibration"
        self.calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def compile_backend(self, category, name, source, *flags):
        target = self.backends / category / name / "backend.so"
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                        "-I", str(SRC / "include"), *flags, str(source), "-o", str(target)],
                       check=True, capture_output=True, text=True)
        return target

    def generate(self, config, directory):
        return subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "generate_config.py"),
             str(config), "--directory", str(directory)],
            text=True, capture_output=True, env=self.env, timeout=60,
        )

    def test_listing_all_plugins_and_filter_without_detection(self):
        for category in ("cpu", "gpu"):
            self.compile_backend(category, "template",
                                 ROOT / "src" / "backend_templates" / f"{category}.c")
        self.compile_backend("cpu", "legacy", SRC / "tests" / "fake_backend.c",
                             '-DFAKE_NAME="Legacy"', "-DFAKE_PRIORITY=0", "-DFAKE_DETECT=0")
        result = subprocess.run([str(SRC / "main"), "--list-metrics"], env=self.env,
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cpu/template: CPU template", result.stdout)
        self.assertIn("gpu/template: GPU template", result.stdout)
        self.assertIn("cpu/legacy: Legacy", result.stdout)
        self.assertIn("Function: backend_energy", result.stdout)
        self.assertIn("Description: Device energy", result.stdout)
        self.assertIn("Origin: Implement hardware counter source", result.stdout)
        self.assertIn("no metric descriptions or origins", result.stderr)
        result = subprocess.run([str(SRC / "main"), "--list-metrics", "gpu", "template"],
                                env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gpu/template", result.stdout)
        self.assertNotIn("cpu/", result.stdout)
        result = subprocess.run([str(SRC / "main"), "--list-metrics", "cpu", "missing"],
                                env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No matching compiled backend", result.stderr)

    def test_listing_reports_plugin_errors_and_honors_filters(self):
        self.compile_backend("cpu", "valid",
                             ROOT / "src" / "backend_templates" / "cpu.c")
        broken = self.backends / "gpu" / "broken" / "backend.so"
        broken.parent.mkdir(parents=True)
        broken.write_text("not a shared library", encoding="utf-8")
        (self.backends / "gpu" / "unbuilt").mkdir()

        source = self.directory / "invalid_info.c"
        source.write_text(
            (SRC / "tests" / "fake_backend.c").read_text(encoding="utf-8")
            + '\nconst EnergyMetricInfoMap energy_metric_info_map = { 999, 0, NULL };\n',
            encoding="utf-8",
        )
        self.compile_backend("gpu", "bad_info", source,
                             '-DFAKE_NAME="Invalid info"', "-DFAKE_PRIORITY=0",
                             "-DFAKE_DETECT=0", "-DFAKE_GPU")
        result = subprocess.run([str(SRC / "main"), "--list-metrics"], env=self.env,
                                text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gpu backend broken: load failed", result.stderr)
        self.assertIn("gpu backend unbuilt: not built", result.stderr)
        self.assertIn("Invalid metric information for gpu/bad_info", result.stderr)

        result = subprocess.run([str(SRC / "main"), "--list-metrics", "cpu", "valid"],
                                env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cpu/valid: CPU template", result.stdout)
        self.assertNotIn("gpu", result.stderr)

    def test_single_registry_entry_is_selectable_by_short_name_or_function(self):
        source = (ROOT / "src" / "tests" / "fixtures" / "toy_backends" / "cpu" / "toy_cpu" / "toy_cpu.c").read_text()
        start = source.index("static const EnergyMetricDefinition toy_cpu_metric_definitions[]")
        end = source.index("ENERGY_DEFINE_METRIC_TABLE(toy_cpu, toy_cpu_metric_definitions)", start)
        end += len("ENERGY_DEFINE_METRIC_TABLE(toy_cpu, toy_cpu_metric_definitions)")
        definitions = """
static double added_metric(int device, pid_t pid) {
    (void)device; (void)pid; return 42.0;
}
#define LEGACY_METRICS(X) \\
    X("Extra", "units", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE, added_metric, "Example extra metric", "Test source") \\
    X("Energy", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE, toy_cpu_metric_energy, "Energy", "Test") \\
    X("LoadAvg", "1m", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_NONE, toy_cpu_metric_load, "Load", "Test") \\
    X("Temperature", "C", ENERGY_METRIC_GAUGE, ENERGY_METRIC_DEVICE, ENERGY_METRIC_FLAG_CPU_TEMPERATURE, toy_cpu_metric_temperature, "Temperature", "Test")
ENERGY_DEFINE_METRICS(toy_cpu, LEGACY_METRICS)
"""
        source = source[:start] + definitions + source[end:]
        file = self.directory / "custom.c"
        file.write_text(source, encoding="utf-8")
        self.compile_backend("cpu", "custom", file)
        listing = subprocess.run([str(SRC / "main"), "--list-metrics", "cpu", "custom"],
                                 env=self.env, text=True, capture_output=True)
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("cpu.Extra (units)", listing.stdout)
        self.assertIn("Function: added_metric", listing.stdout)
        self.assertIn("Description: Example extra metric", listing.stdout)
        self.assertIn("Origin: Test source", listing.stdout)
        for mode in ("standard", "overtime"):
            for selection in ("cpu.Extra", "cpu.added_metric"):
                with self.subTest(mode=mode, selection=selection):
                    output = self.directory / "result.csv"
                    config = self.directory / "config.toml"
                    config.write_text(
                        f'[measurement]\nmode="{mode}"\ncommand="sleep 0.05"\n'
                        f'output="{output}"\ncalibration_file="{self.calibration}"\n'
                        f'variance=100\nsleep_seconds=0\n[metrics]\nselect=["{selection}"]\n',
                        encoding="utf-8",
                    )
                    build = self.directory / f"{mode}-{selection}"
                    result = self.generate(config, build)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(build / "greenmeasurer")], env=self.env,
                                            text=True, capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    with output.open(newline="", encoding="utf-8") as file:
                        rows = list(csv.reader(file, skipinitialspace=True))
                    self.assertEqual(rows[0][-1], "cpu.Extra_units")
                    self.assertTrue(all(float(row[-1]) == 42 for row in rows[1:]))
                    output.unlink()

    def test_structured_metric_entry_is_listed_and_selectable(self):
        source = (SRC / "tests" / "fixtures" / "toy_backends" / "cpu" /
                  "toy_cpu" / "toy_cpu.c").read_text(encoding="utf-8")
        declaration = "static const EnergyMetricDefinition toy_cpu_metric_definitions[] = {"
        self.assertIn(declaration, source)
        source = source.replace(declaration, """
static double added_metric(int device, pid_t pid) {
    (void)device; (void)pid; return 42.0;
}
""" + declaration + """
    {
        .descriptor = {.name = "Extra", .unit = "units", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(added_metric),
        .info = {.description = "Example extra metric", .origin = "Test source"}
    },
""", 1)
        file = self.directory / "structured.c"
        file.write_text(source, encoding="utf-8")
        self.compile_backend("cpu", "structured", file)
        listing = subprocess.run([str(SRC / "main"), "--list-metrics", "cpu", "structured"],
                                 env=self.env, text=True, capture_output=True)
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("cpu.Extra (units)", listing.stdout)
        self.assertIn("Function: added_metric", listing.stdout)
        self.assertIn("Description: Example extra metric", listing.stdout)
        self.assertIn("Origin: Test source", listing.stdout)
        for mode in ("standard", "overtime"):
            for selection in ("cpu.Extra", "cpu.added_metric"):
                with self.subTest(mode=mode, selection=selection):
                    output = self.directory / "structured.csv"
                    config = self.directory / "structured.toml"
                    config.write_text(
                        f'[measurement]\nmode="{mode}"\ncommand="sleep 0.05"\n'
                        f'output="{output}"\ncalibration_file="{self.calibration}"\n'
                        f'variance=100\nsleep_seconds=0\n[metrics]\nselect=["{selection}"]\n',
                        encoding="utf-8",
                    )
                    build = self.directory / f"structured-{mode}-{selection}"
                    result = self.generate(config, build)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(build / "greenmeasurer")], env=self.env,
                                            text=True, capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    with output.open(newline="", encoding="utf-8") as file:
                        rows = list(csv.reader(file, skipinitialspace=True))
                    self.assertEqual(rows[0][-1], "cpu.Extra_units")
                    self.assertTrue(rows[1:])
                    self.assertTrue(all(float(row[-1]) == 42 for row in rows[1:]))
                    output.unlink()

    def test_intel_structured_metric_can_be_added(self):
        intel = SRC / "platform" / "linux" / "cpu" / "intel"
        if not (intel / "backend.so").exists():
            self.skipTest("Intel backend development dependencies are unavailable")
        source = (intel / "intel.c").read_text(encoding="utf-8")
        declaration = "static const EnergyMetricDefinition intel_metric_definitions[] = {"
        self.assertIn(declaration, source)
        source = source.replace(
            declaration,
            "static double added_metric(int device, pid_t pid) "
            "{ (void)device; (void)pid; return 42.0; }\n" + declaration + """
    {
        .descriptor = {.name = "Extra", .unit = "units", .kind = ENERGY_METRIC_GAUGE,
                       .scope = ENERGY_METRIC_DEVICE, .flags = ENERGY_METRIC_FLAG_NONE},
        .function = ENERGY_METRIC_FUNCTION(added_metric),
        .info = {.description = "Example extra metric", .origin = "Test source"}
    },
""", 1,
        )
        file = self.directory / "intel-with-extra.c"
        file.write_text(source, encoding="utf-8")
        plugin = self.backends / "cpu" / "intel-test" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
             "-I", str(SRC / "include"), "-I", str(intel), str(file), "-o", str(plugin), "-lm"],
            check=True, capture_output=True, text=True,
        )
        listing = subprocess.run([str(SRC / "main"), "--list-metrics", "cpu", "intel-test"],
                                 env=self.env, text=True, capture_output=True)
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("cpu.Extra (units)", listing.stdout)
        self.assertIn("Function: added_metric", listing.stdout)
        self.assertIn("Description: Example extra metric", listing.stdout)
        self.assertIn("cpu.Package (J)", listing.stdout)
        self.assertIn("cpu.Temperature (C)", listing.stdout)

    def test_fibonacci_examples_execute_both_modes(self):
        demo = self.directory / "fibonacci-demo"
        demo.mkdir()
        binary = demo / "fibonacci"
        subprocess.run(["cc", "-O0", str(ROOT / "Languages" / "C" / "Fibonacci" / "fibonacci.c"),
                        "-o", str(binary)], check=True, capture_output=True, text=True)
        self.compile_backend("cpu", "fake", SRC / "tests" / "fake_backend.c",
                             '-DFAKE_NAME="Fake"', "-DFAKE_PRIORITY=0", "-DFAKE_DETECT=1")
        empty_backends = self.directory / "empty-backends"
        empty_backends.mkdir()
        for mode in ("standard", "overtime"):
            example = ROOT / "src" / "tests" / "fixtures" / "configurations" / f"fibonacci-{mode}.toml"
            output = self.directory / f"fibonacci-{mode}.csv"
            build = self.directory / f"build-{mode}"
            result = self.generate(example, build)
            self.assertEqual(result.returncode, 0, result.stderr)
            for package_available in (False, True):
                with self.subTest(mode=mode, package_available=package_available):
                    environment = self.env.copy()
                    environment["GREENMEASURER_BACKEND_DIR"] = str(
                        self.backends if package_available else empty_backends)
                    baseline = demo / "temperature.txt"
                    if baseline.exists():
                        baseline.unlink()
                    self.assertFalse(output.exists())
                    result = subprocess.run([str(build / "greenmeasurer")], env=environment,
                                            cwd=self.directory, text=True, capture_output=True,
                                            timeout=60)
                    if not package_available:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertFalse(baseline.exists())
                        self.assertFalse(output.exists())
                        self.assertIn("CPU temperature is unavailable", result.stderr)
                        continue
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn("DETAILS", result.stdout)
                    self.assertIn("  TYPE  BACKEND                       STATUS", result.stdout)
                    self.assertTrue(baseline.exists())
                    self.assertTrue(output.stat().st_mode & 0o044, "CSV must be readable by other users")
                    with output.open(newline="", encoding="utf-8") as file:
                        rows = list(csv.reader(file, skipinitialspace=True))
                    columns = ["Language", "Program", "Powercap",
                               "Time_ms" if mode == "standard" else "Timestamp_ms", "Cores"]
                    if package_available:
                        columns.append("cpu.Package_J")
                    self.assertEqual(rows[0], columns)
                    if mode == "standard":
                        self.assertEqual(len(rows), 4)
                    self.assertGreater(len(rows), 1)
                    self.assertTrue(all(len(row) == len(columns) for row in rows))
                    output.unlink()

    def test_gpu_only_run_does_not_require_cpu_temperature(self):
        self.compile_backend("gpu", "fake", SRC / "tests" / "fake_backend.c",
                             '-DFAKE_NAME="FakeGpu"', "-DFAKE_PRIORITY=0", "-DFAKE_DETECT=1",
                             "-DFAKE_GPU")
        output = self.directory / "gpu-only.csv"
        config = self.directory / "gpu-only.toml"
        config.write_text(
            '[measurement]\nmode = "standard"\ncommand = "true"\n'
            f'output = "{output}"\nsleep_seconds = 0\n'
            f'calibration_file = "{self.directory / "missing-baseline.txt"}"\n\n'
            '[metrics]\nselect = ["system.cores"]\n', encoding="utf-8")
        build = self.directory / "gpu-only-build"
        result = self.generate(config, build)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(build / "greenmeasurer")], env=self.env,
                                cwd=self.directory, text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("CPU temperature is unavailable", result.stderr)
        self.assertTrue(output.exists())


if __name__ == "__main__":
    unittest.main()
