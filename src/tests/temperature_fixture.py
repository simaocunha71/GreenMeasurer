import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def install_temperature_backend(directory):
    plugin = directory / "cpu" / "test_temperature" / "backend.so"
    if plugin.exists():
        return
    plugin.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
         str(ROOT / "src" / "tests" / "temperature_backend.c"), "-o", str(plugin)],
        check=True, capture_output=True, text=True,
    )
