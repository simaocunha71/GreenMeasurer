import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "src" / "tests" / "legacy-engine"


class TemperatureRequiredTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "src"), "tests/legacy-engine"],
                       check=True, capture_output=True, text=True)

    def test_missing_temperature_stops_every_mode_before_workload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            backends = root / "backends"
            backends.mkdir()
            marker = root / "executed"
            calibration = root / "temperature.txt"
            calibration.write_text("Average temperature: 40.0\n", encoding="utf-8")
            output = root / "output.csv"
            env = os.environ.copy()
            env["GREENMEASURER_BACKEND_DIR"] = str(backends)
            for mode in ("--configured-standard", "--configured-over-time",
                         "--temperature-calibration", "--powercap-calibration"):
                with self.subTest(mode=mode):
                    arguments = [str(ENGINE), mode, "-command", f"touch {marker}",
                                 "-calib-file", str(calibration), "-output", str(output)]
                    if mode.startswith("--configured"):
                        arguments.extend(["--configured-system", "system.cores"])
                    if mode == "--powercap-calibration":
                        arguments.extend(["-range", "1-2"])
                    result = subprocess.run(arguments, env=env, text=True,
                                            capture_output=True, timeout=15)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(marker.exists())
                    self.assertFalse(output.exists())
                    self.assertIn("CPU temperature is unavailable", result.stderr)

    def test_unavailable_or_lost_temperature_stops_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            backend = root / "cpu" / "fake" / "backend.so"
            backend.parent.mkdir(parents=True)
            marker = root / "executed"
            baseline = root / "baseline"
            baseline.write_text("Average temperature: 40.0\n", encoding="utf-8")
            env = os.environ.copy()
            env["GREENMEASURER_BACKEND_DIR"] = str(root)
            for failure in ("FAKE_TEMPERATURE_UNAVAILABLE", "FAKE_TEMPERATURE_NAN",
                            "FAKE_TEMPERATURE_FAIL_AFTER_ONE"):
                with self.subTest(failure=failure):
                    subprocess.run(
                        ["cc", "-fPIC", "-shared", '-DFAKE_NAME="No temperature"',
                         "-DFAKE_PRIORITY=0", "-DFAKE_DETECT=1", f"-D{failure}",
                         str(ROOT / "src" / "tests" / "fake_backend.c"), "-o", str(backend)],
                        check=True, capture_output=True, text=True,
                    )
                    result = subprocess.run(
                        [str(ENGINE), "--configured-standard", "-command", f"touch {marker}",
                         "-calib-file", str(baseline), "-output", str(root / "output.csv"),
                         "--configured-system", "system.cores"],
                        env=env, text=True, capture_output=True, timeout=15,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("CPU temperature is unavailable", result.stderr)
                    self.assertFalse(marker.exists())
                    self.assertFalse((root / "output.csv").exists())


if __name__ == "__main__":
    unittest.main()
