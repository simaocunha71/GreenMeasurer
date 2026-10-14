# GreenMeasurer: A Hardware-Agnostic Energy Telemetry & Benchmarking Framework

GreenMeasurer is a modular, low-overhead measurement framework designed to profile software energy consumption, power capping curves, and execution telemetry across diverse architectures.

The tool coordinates platform-specific telemetry drivers behind a generic **Hardware Abstraction Layer (HAL)** in C, capturing unified energy readouts (Joules), peak memory usage, and thermal states across CPU and GPU hardware domains.

| Authors      | Contacts                 |
|:------------:|:------------------------:|
| Simão Cunha  | simao.s.cunha@inesctec.pt |
| João Saraiva | saraiva@di.uminho.pt     |

---

## Architecture & Directory Layout

The project isolates platform access from measurement orchestration. All platform implementations reside under a modular hierarchy:

```text
src/
├── include/
│   ├── energy-backend.h                # HAL definition for CPU & GPU energy backends
│   └── sensors.h                       # Abstraction drivers for thermal sensors
├── platform/
│   └── linux/
│       ├── cpu/
│       │   ├── amd/
│       │   │   ├── amd.c               # AMD Zen MSR counter acquisition & 32-bit wrap delta
│       │   │   ├── amd.h               # AMD Zen MSR definitions & constants
│       │   │   └── sensors.c           # k10temp (Tctl/Tdie) sensor driver
│       │   └── intel/
│       │       ├── intel.c             # Intel RAPL MSR telemetry & raplcap powercapping
│       │       ├── intel.h             # Intel MSR addresses & model declarations
│       │       └── sensors.c           # coretemp sensor driver & package temperature
│       └── gpu/
│           └── nvidia/
│               ├── nvidia.c            # NVIDIA NVML energy counter interface
│               ├── nvidia.h            # NVIDIA backend interface
│               └── sensors.c           # NVIDIA NVML thermal querying
├── sensors/
│   └── sensors.c                       # Shared runtime sensor dispatcher & calibration
├── main.c                              # Main benchmark runner, CLI parser & overtime sampler
└── Makefile                            # Architecture-aware dynamic build system
```

---

## Supported Hardware & Features

| Platform Target | Energy Telemetry | Power Capping | Thermal Sensors | Driver / Mechanism |
|---|---|---|---|---|
| **Intel CPU** | Package, Core, DRAM, GPU (iGPU) | Supported (`raplcap` / `powercap`) | Core average & Package | Model-Specific Registers (`/dev/cpu/%d/msr`) |
| **AMD Zen CPU** | Package, Core | Baseline only | Tctl / Tdie | Zen MSRs with 32-bit overflow handling |
| **NVIDIA GPU** | Dedicated GPU domain | Supported by NVML API | Core junction | NVML (`libnvidia-ml`) |

> **Note on CSV Semantics:** Any metric not exposed or unsupported on the running machine reports `NOT_SUPPORTED` (backed by negative sentinel values internally), preventing false zero-consumption (`0.0 J`) distortions.

---

## System Requirements & Prerequisites

### 1. Operating System & Kernel
* Linux Kernel 4.x or later.
* Access to kernel MSRs:
  ```bash
  sudo modprobe msr
  ```

### 2. Dependencies
* `build-essential` (`gcc`, `make`)
* `lm-sensors`
* `libpowercap-dev` and `libraplcap-dev` (Intel power clamping)
* `libnvidia-ml-dev` or NVIDIA proprietary driver with CUDA/NVML headers (when NVIDIA GPUs are present)

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential lm-sensors libpowercap-dev libraplcap-dev
```

---

## Building the Framework

The conditional `Makefile` inspects `/proc/cpuinfo` and `lspci` to build only the modules compatible with your host:

```bash
# Clean previous build artifacts
make clean

# Compile the main executable
make
```

---

## CLI Modes & Command Reference

The executable supports four core operating modes:

### 1. Temperature Calibration (`--temperature-calibration`)
Establishes an idle thermal reference point used to enforce temperature variance checks prior to execution iterations.

```bash
sudo ./main --temperature-calibration [-sleep-secs <sec>] [-calib-file <path>]
```

* `-sleep-secs <sec>`: Idle duration in seconds before recording temperature (default: `20`).
* `-calib-file <path>`: Output destination for the thermal reference profile (default: `/tmp/cores_temperatures.txt`).

---

### 2. Standard Aggregated Measurement (`--standard`)
Runs repeated benchmark iterations with timeout tracking, memory profiling, and thermal settling.

```bash
sudo ./main --standard \
  -command "<command>" \
  -language <lang> \
  -program <prog> \
  -n_times <N> \
  -variance <V> \
  -time_out_limit <hours> \
  -sleep_time <sec> \
  -powercap <W> \
  [-output <path>]
```

#### Parameters:
* `-command`: Execution string (e.g., `"./binary arg"` or `"python3 script.py"`).
* `-language`: Tag identifying language / implementation.
* `-program`: Identifier for the tested target.
* `-n_times`: Repetitions to run.
* `-variance`: Allowed °C threshold above baseline before triggering execution.
* `-time_out_limit`: Maximum allowed duration per run in hours.
* `-sleep_time`: Stabilization sleep duration.
* `-powercap`: Package clamping limit in Watts (`-1` to disable).
* `-output`: Output CSV file path (default: `measurements.csv`).

---

### 3. Over-Time Dynamic Sampling (`--overtime`)
Spawns an asynchronous background logger that records cumulative energy and thermal changes using non-drifting absolute monotonic ticks (`clock_nanosleep`).

```bash
sudo ./main --overtime \
  -command "<command>" \
  -interval <ms> \
  -language <lang> \
  -program <prog> \
  [-powercap <W>] \
  [-variance <V>] \
  [-sleep-secs <sec>] \
  [-output <path>]
```

#### Parameters:
* `-command`: Foreground benchmark workload.
* `-interval`: Sampling step in milliseconds (e.g., `100` for 10 Hz).
* `-language`: Programming language identifier.
* `-program`: Program identifier.
* `-powercap`: Clamping limit in Watts (optional, defaults to `-1`).
* `-variance`: Maximum allowed temperature variance above baseline.
* `-sleep-secs`: Stabilization sleep duration.
* `-output`: Output CSV file path (default: `measurements_ot.csv`).

---

### 4. Powercap Curve Exploration (`--powercap-calibration`)
Sweeps a power range ($W_{\min} \dots W_{\max}$) to evaluate powercap impacts against an uncapped baseline.

```bash
sudo ./main --powercap-calibration \
  -range <min>-<max> \
  -variance <V> \
  -time_out_limit <hours> \
  -secs_to_sleep <sec> \
  -n_times <N> \
  [-compile_cmd "<cmd>"] \
  -command "<cmd>" \
  -language <lang> \
  -program <prog> \
  [-output <path>]
```

Runs a baseline iteration at `Powercap = -1`, followed by tests at each Watt step in the defined range. Results are saved to `powercap_calibration.csv` and summarized automatically if post-processing scripts are present.

---

## Output CSV Schemas

### Standard Mode (`measurements.csv`)
```csv
Language,Program,Powercap,Package,Core,GPU,DRAM,Time,CpuTemperature,GpuTemperature,Memory
```
* **Language / Program**: Identifier tags.
* **Powercap**: Power limit applied in Watts (`-1` = uncapped).
* **Package**: Full socket energy in Joules.
* **Core**: Processing cores energy in Joules.
* **GPU**: GPU energy in Joules.
* **DRAM**: Memory domain energy in Joules.
* **Time**: Execution wall-clock duration in milliseconds ($ms$).
* **CpuTemperature**: Core temperature in Celsius ($^\circ\text{C}$).
* **GpuTemperature**: GPU temperature in Celsius ($^\circ\text{C}$).
* **Memory**: Resident Set Size in kilobytes ($KB$).

### Over-Time Mode (`measurements_ot.csv`)
```csv
Language,Program,Powercap,Package,Core,GPU,DRAM,Timestamp,CpuTemperature,GpuTemperature
```
* **Package, Core, GPU, DRAM**: Total accumulated energy from $T = 0$ to current sample (in Joules).
* **Timestamp**: Elapsed time since start in milliseconds ($ms$).
* **CpuTemperature / GpuTemperature**: Instantaneous sensor reading at sampling tick.

---

## Adding a New Architecture / Driver

The HAL enables hardware support extensions without modifying `main.c`:

1. **Implement `EnergyBackend`**: Place your driver inside `platform/linux/<type>/<vendor>/`:
   ```c
   #include "energy-backend.h"

   static int custom_detect(void) { /* return 1 if present */ return 1; }
   static int custom_before(int core) { return 1; }
   static int custom_after(int core, EnergyMetrics *out) {
       out->package = 15.2; // Energy delta in Joules
       out->core = -1.0;    // NOT_SUPPORTED
       out->gpu = -1.0;
       out->dram = -1.0;
       return 1;
   }

   EnergyBackend custom_backend = {
       .name = "Custom Architecture",
       .detect = custom_detect,
       .before = custom_before,
       .after = custom_after
   };
   ```

2. **Expose Symbol**: Add the weak reference to `include/energy-backend.h`:
   ```c
   WEAK_DRIVER extern EnergyBackend custom_backend;
   ```

3. **Register in `Makefile`**: Append your source file to `SRCS` under the corresponding hardware conditional branch.