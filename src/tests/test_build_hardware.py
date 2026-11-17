import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"


class BuildHardwareTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.cpuinfo = self.directory / "cpuinfo"
        self.pci = self.directory / "pci"
        self.pci.mkdir()
        self.environment = os.environ.copy()
        self.environment["GREENMEASURER_CPUINFO"] = str(self.cpuinfo)
        self.environment["GREENMEASURER_PCI_DEVICES"] = str(self.pci)

    def tearDown(self):
        self.temporary.cleanup()

    def cpu(self, vendor):
        self.cpuinfo.write_text(f"processor : 0\nvendor_id : {vendor}\n", encoding="ascii")

    def device(self, name, vendor, device_class):
        device = self.pci / name
        device.mkdir()
        (device / "vendor").write_text(f"0x{vendor:04x}\n", encoding="ascii")
        (device / "class").write_text(f"0x{device_class:06x}\n", encoding="ascii")

    def targets(self, *arguments):
        return subprocess.run(
            ["make", "-s", "-C", str(SRC), "--eval",
             "print-test-targets: ; @printf '%s\\n' $(BACKEND_TARGETS)",
             "print-test-targets",
             "BACKEND_AVAILABLE_platform_linux_cpu_intel=yes",
             "BACKEND_AVAILABLE_platform_linux_gpu_nvidia=yes", *arguments],
            env=self.environment, text=True, capture_output=True, timeout=30,
        )

    def test_selects_backends_for_detected_hardware(self):
        self.cpu("GenuineIntel")
        self.device("network", 0x10DE, 0x020000)
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), ["platform/linux/cpu/intel/backend.so"])

        self.device("intel-gpu", 0x8086, 0x030000)
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), [
            "platform/linux/cpu/intel/backend.so", "platform/linux/gpu/intel/backend.so",
        ])

        self.device("nvidia-gpu", 0x10DE, 0x030200)
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), [
            "platform/linux/cpu/intel/backend.so", "platform/linux/gpu/intel/backend.so",
            "platform/linux/gpu/nvidia/backend.so",
        ])

        self.cpu("AuthenticAMD")
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), [
            "platform/linux/gpu/intel/backend.so", "platform/linux/gpu/nvidia/backend.so",
        ])

    def test_missing_dependencies_still_skip_detected_backend(self):
        self.cpu("GenuineIntel")
        self.device("nvidia-gpu", 0x10DE, 0x030000)
        result = self.targets("BACKEND_AVAILABLE_platform_linux_gpu_nvidia=no")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), ["platform/linux/cpu/intel/backend.so"])

    def test_build_commands_only_include_detected_backends(self):
        self.cpu("GenuineIntel")
        for add_gpu in (False, True):
            with self.subTest(intel_gpu=add_gpu):
                if add_gpu:
                    self.device("intel-gpu", 0x8086, 0x030000)
                result = subprocess.run(
                    ["make", "-n", "-B", "-C", str(SRC), "all",
                     "BACKEND_AVAILABLE_platform_linux_cpu_intel=yes",
                     "BACKEND_AVAILABLE_platform_linux_gpu_nvidia=yes"],
                    env=self.environment, text=True, capture_output=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("-o platform/linux/cpu/intel/backend.so", result.stdout)
                self.assertEqual("-o platform/linux/gpu/intel/backend.so" in result.stdout, add_gpu)
                self.assertNotIn("-o platform/linux/gpu/nvidia/backend.so", result.stdout)

    def test_detection_failure_warns_and_skips_gpu_backends(self):
        self.cpu("GenuineIntel")
        self.environment["GREENMEASURER_PCI_DEVICES"] = str(self.directory / "missing")
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), ["platform/linux/cpu/intel/backend.so"])
        self.assertIn("Hardware detector for platform/linux/gpu/intel failed", result.stderr)
        self.assertIn("Hardware detector for platform/linux/gpu/nvidia failed", result.stderr)

    def test_custom_backend_detection_can_be_configured_in_make(self):
        self.cpu("AuthenticAMD")
        custom = self.directory / "custom"
        custom.mkdir()
        result = self.targets(f"BACKEND_DIRS={custom}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stdout.strip())
        self.assertIn("no hardware detector configured", result.stderr)

        detector = f"BACKEND_DETECT_{str(custom).replace('/', '_')}"
        result = self.targets(f"BACKEND_DIRS={custom}",
                              f"{detector}=$(call detect_cpu_vendor,AuthenticAMD)")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(custom / "backend.so"))

        result = self.targets(f"BACKEND_DIRS={custom}",
                              f"{detector}=$(call detect_cpu_vendor,GenuineIntel)")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stdout.strip())

        self.device("custom-gpu", 0x1234, 0x030000)
        result = self.targets(f"BACKEND_DIRS={custom}",
                              f"{detector}=$(call detect_pci_gpu_vendor,0x1234)")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(custom / "backend.so"))

        result = self.targets(f"BACKEND_DIRS={custom}", f"{detector}=exit 2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stdout.strip())
        self.assertIn("Hardware detector for", result.stderr)

    def test_invalid_hardware_information_warns(self):
        self.cpuinfo.write_text("processor : 0\n", encoding="ascii")
        self.device("broken", 0x8086, 0x030000)
        (self.pci / "broken" / "class").write_text("invalid\n", encoding="ascii")
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stdout.strip())
        self.assertIn("Hardware detector for platform/linux/cpu/intel failed", result.stderr)
        self.assertIn("Invalid PCI device information", result.stderr)
        self.assertIn("Hardware detector for platform/linux/gpu/intel failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
