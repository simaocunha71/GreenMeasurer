import csv
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts" / "generate_config.py"
CUSTOM = ROOT / "configurations" / "new-metrics"


class PowercapSweepTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "src"), "all"], check=True,
                       capture_output=True, text=True)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.backends = self.directory / "backends"
        plugin = self.backends / "cpu" / "recorder" / "backend.so"
        plugin.parent.mkdir(parents=True)
        subprocess.run(
            ["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
             str(ROOT / "src" / "tests" / "powercap_backend.c"), "-o", str(plugin)],
            check=True, capture_output=True, text=True,
        )
        self.log = self.directory / "powercap.log"
        self.calibration = self.directory / "calibration.txt"
        self.calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
        self.environment = os.environ.copy()
        self.environment["GREENMEASURER_BACKEND_DIR"] = str(self.backends)
        self.environment["POWERCAP_LOG"] = str(self.log)
        self.output = self.directory / "result.csv"

    def tearDown(self):
        self.temporary.cleanup()

    def run_config(self, text, name="config.toml"):
        config = self.directory / name
        config.write_text(text, encoding="utf-8")
        self.builds = getattr(self, "builds", 0) + 1
        build = self.directory / f"build{self.builds}"
        generated = subprocess.run(
            [sys.executable, str(GENERATOR), str(config), "--directory", str(build)],
            text=True, capture_output=True, env=self.environment, timeout=120,
        )
        self.assertEqual(generated.returncode, 0, generated.stderr)
        return subprocess.run([str(build / "greenmeasurer")], cwd=self.directory, text=True,
                              capture_output=True, env=self.environment, timeout=120)

    def measurement(self, mode, extra=""):
        return (f'[measurement]\nmode="{mode}"\ncommand="true"\noutput="{self.output}"\n'
                f'calibration_file="{self.calibration}"\nsleep_seconds=0\nvariance=100\n{extra}')

    def rows(self):
        with self.output.open(newline="", encoding="utf-8") as file:
            return list(csv.reader(file, skipinitialspace=True))

    def operations(self):
        return self.log.read_text(encoding="utf-8").split("\n")[:-1] if self.log.exists() else []

    def log_temperature_reads(self):
        self.environment["POWERCAP_LOG_TEMPERATURE"] = "1"

    def test_standard_runs_every_requested_powercap_and_restores_limits(self):
        self.log_temperature_reads()
        result = self.run_config(
            self.measurement("standard", "repetitions=2\npowercap=[-1, 5, 6]\n") +
            '[metrics]\nselect=["cpu.Package"]\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows()
        self.assertEqual(rows[0][:4], ["Language", "Program", "Powercap", "Time_ms"])
        self.assertEqual([row[2] for row in rows[1:]], ["-1", "-1", "5", "5", "6", "6"])
        self.assertEqual(self.operations(), [
            "temperature 0", "set -1", "temperature 0", "temperature 0", "restore 0",
            "set 5", "temperature 0", "temperature 0", "restore 0",
            "set 6", "temperature 0", "temperature 0", "restore 0",
        ])

    def test_single_standard_powercap_is_restored(self):
        result = self.run_config(
            self.measurement("standard", "powercap=5\n") + '[metrics]\nselect=["cpu.Package"]\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.operations(), ["set 5", "restore 0"])

    def test_overtime_appends_every_powercap_to_one_csv(self):
        self.log_temperature_reads()
        result = self.run_config(
            self.measurement("over-time", "interval_ms=1\npowercap=[-1, 5, 6]\n") +
            '[metrics]\nselect=["cpu.Package"]\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows()
        self.assertEqual(sum(row[0] == "Language" for row in rows), 1)
        caps = [row[2] for row in rows[1:]]
        self.assertEqual(set(caps), {"-1", "5", "6"})
        self.assertEqual(caps, sorted(caps))
        self.assertEqual(self.operations(), [
            "temperature 0", "set -1", "temperature 0", "restore 0",
            "set 5", "temperature 0", "restore 0",
            "set 6", "temperature 0", "restore 0",
        ])

    def test_invalid_powercap_lists_are_rejected_before_generation(self):
        for value in ("[5, 5]", "[]", "[0]", "[-2]", '"5"'):
            with self.subTest(value=value):
                config = self.directory / "invalid.toml"
                config.write_text(self.measurement("standard", f"powercap={value}\n") +
                                  '[metrics]\nselect=["cpu.Package"]\n', encoding="utf-8")
                result = subprocess.run(
                    [sys.executable, str(GENERATOR), str(config), "--directory",
                     str(self.directory / "never")],
                    text=True, capture_output=True, env=self.environment, timeout=60)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("powercap", result.stderr)
                self.assertFalse((self.directory / "never").exists())

    def calibration_config(self, metrics):
        return (f'[measurement]\nmode="powercap-calibration"\ncommand="true"\n'
                f'output="{self.output}"\ncalibration_file="{self.calibration}"\n'
                'sleep_seconds=0\nvariance=100\nrepetitions=2\npowercap_min=5\npowercap_max=6\n'
                + metrics)

    def test_calibration_defaults_to_package_metric(self):
        result = self.run_config(self.calibration_config(""))
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows()
        self.assertEqual(rows[0][4:], ["cpu.Package_J"])
        self.assertEqual([row[2] for row in rows[1:]], ["-1", "-1", "5", "5", "6", "6"])
        self.assertEqual(self.operations(), [
            "set -1", "restore 0",
            "set 5", "restore 0",
            "set 6", "restore 0",
        ])

    def test_calibration_accepts_selected_all_and_new_metrics(self):
        result = self.run_config(self.calibration_config('[metrics]\nselect=["system.cores", "cpu.Temperature"]\n'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.rows()[0][4:], ["Cores", "cpu.Temperature_C"])

        result = self.run_config(self.calibration_config('[metrics]\nselect="all"\n'))
        self.assertEqual(result.returncode, 0, result.stderr)
        header = self.rows()[0]
        self.assertIn("cpu.Package_J", header)
        self.assertIn("Cores", header)

        new_metric = (
            '[metrics]\nselect=["cpu.Package", "external.load.LoadAverage1m"]\n'
            '[[external_metrics]]\nname="load"\nmetric="LoadAverage1m"\nunit="load"\n'
            'function="get_load_average_1min"\n'
            f'header="{CUSTOM / "load_average.h"}"\nsources=["{CUSTOM / "load_average.c"}"]\n'
            'return_type="double"\n')
        result = self.run_config(self.calibration_config(new_metric))
        self.assertEqual(result.returncode, 0, result.stderr)
        header = self.rows()[0]
        self.assertEqual(header[4:], ["cpu.Package_J", "external.load.LoadAverage1m_load"])

    def test_temperature_calibration_runs_directly_without_generating_files(self):
        target = self.directory / "baseline.txt"
        config = self.directory / "temperature.toml"
        config.write_text(f'[measurement]\nmode="temperature-calibration"\nsleep_seconds=0\n'
                          f'calibration_file="{target}"\n', encoding="utf-8")
        result = subprocess.run([sys.executable, str(GENERATOR), str(config)], text=True,
                                capture_output=True, env=self.environment, timeout=60,
                                cwd=self.directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Average temperature:", target.read_text(encoding="utf-8"))
        self.assertEqual(sorted(path.name for path in self.directory.iterdir()),
                         ["backends", "baseline.txt", "calibration.txt", "temperature.toml"])
        result = subprocess.run(
            [str(ROOT / "src" / "main"), "--config", str(config)], text=True,
            capture_output=True, env=self.environment, timeout=60, cwd=self.directory)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
