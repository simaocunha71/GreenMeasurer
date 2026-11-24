# Device Libraries

This directory contains the C libraries that expose hardware and system
measurement functions (as well as temperature calibration and power-cap control
hooks) to GreenMeasurer aspects.

## Directory layout

```
src/devices/
├── Makefile
├── README.md
└── linux/
    ├── msr.c / msr.h                 # Low-level MSR access helpers for RAPL readers
    ├── system.c / system.h           # OS-level system metrics (cores, running processes)
    ├── wall_clock_time.c / .h        # CLOCK_MONOTONIC duration callbacks and reader
    ├── cpu/
    │   └── intel/
    │       ├── intel_cpu.c / .h                  # Intel RAPL energy readers
    │       ├── intel_cpu_temperature.c / .h      # Coretemp reader & calibrator (lm-sensors)
    │       ├── intel_cpu_powercap.c / .h         # RAPLCap power-cap control & calibration
    │       └── processPowercapCalibrationCSV.py  # GPS-UP report & optimal power-cap finder
    └── gpu/
        ├── intel/
        │   └── intel_gpu.c / .h      # Intel integrated GPU RAPL energy reader
        └── nvidia/
            └── nvidia_gpu.c / .h     # NVIDIA GPU power/energy/temperature reader (NVML)
```

* `Makefile`: builds static archives (`libgm_*.a`) for all supported backends
  whose development headers are present on the system.
* `processPowercapCalibrationCSV.py`: post-processes power-cap calibration
  CSV file using IQR outlier filtering and the GPS-UP energy efficiency metrics
  (Speedup, Greenup, Powerup), printing a summary report and optionally writing
  the power-cap level with the lowest average Package energy to an output file.

## Building

```sh
make -C src/devices
```

Libraries requiring optional system headers (`sensors/sensors.h`,
`powercap/powercap.h` + `raplcap/raplcap.h`, or `nvml.h`) are automatically
included when those headers are available and skipped otherwise.

## Adding a new device or OS backend

1. Create a directory under `<os>/<kind>/<vendor>/` (or `<os>/` for shared
   utilities) with a `.c` implementation and a `.h` header declaring
   no-argument reader functions (and optional `setup`/`teardown` or control
   hooks).
2. Add a static library target (`libgm_<name>.a`) to `Makefile`.
3. Declare an aspect `.toml` file referencing the generated `.a` archive in
   `device.lib`, the `.h` file in `device.header`, and any required linker flags
   in `device.link`).