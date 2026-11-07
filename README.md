# GreenMeasurer: A Hardware-Agnostic Energy Telemetry & Benchmarking Framework

GreenMeasurer is a modular, low-overhead measurement framework designed to profile software energy consumption, power capping curves, and execution telemetry across diverse architectures.

The tool coordinates platform-specific telemetry drivers behind a generic **Hardware Abstraction Layer (HAL)** in C, capturing unified energy readouts (Joules), peak memory usage, and thermal states across CPU and GPU hardware domains.

| Authors      | Contacts                 |
|:------------:|:------------------------:|
| Simão Cunha  | simao.s.cunha@inesctec.pt |
| João Saraiva | saraiva@di.uminho.pt     |

---

> The research-paper-style documentation follows the section structure of `GreenMeasurer.pdf`: [docs/PAPER_DRAFT.md](docs/PAPER_DRAFT.md). For a supplementary implementation overview, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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
├── metrics/
│   └── system_metrics.c                # Core count, per-core frequency, process counts, RSS
├── main.c                              # Main benchmark runner, CLI parser & overtime sampler
└── Makefile                            # Architecture-aware dynamic build system
```

---

## Supported Hardware & Features

| Platform Target | Energy Telemetry | Power Capping | Thermal Sensors | Driver / Mechanism |
|---|---|---|---|---|
| **Intel CPU** | Package, Core, DRAM, GPU (iGPU) | Supported (`raplcap` / `powercap`) | Core average & Package | Model-Specific Registers (`/dev/cpu/%d/msr`) |
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
* `libpowercap` and `raplcap` headers and libraries (required to build the Intel CPU backend, including its power-capping support)
* NVML headers and `libnvidia-ml` (required to build the NVIDIA GPU backend)

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential lm-sensors libpowercap-dev libraplcap-dev
```

---

## Building the Framework

The `Makefile` checks each optional backend's build dependencies and skips backends whose required headers or libraries are unavailable. The CPU/GPU plugins that do build perform hardware detection at runtime; a plugin that does not match the current hardware is not selected.

```bash
# Clean previous build artifacts
make clean

# Compile the core and all backends whose dependencies are installed
make
```

For example, the Intel CPU backend is skipped when the powercap/raplcap development files are absent, while dependency-free backends can still be built. The NVIDIA backend requires both the NVML header and library. The build prints a reason for each skipped optional backend and continues with the remaining available plugins.

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
The parent process samples the backends and system metrics sequentially while the
workload runs, without sampler threads. Samples are stored in static arrays and
the CSV is written after the workload exits. The default pause between sampling
cycles is 1 ms; the time spent reading metrics and scheduler delays make the
actual intervals longer and variable. This is a polling interval, not a
hardware update notification: repeated counter readings or missed updates are
possible. A final partial interval is recorded when the workload exits.

The capacity is 262144 rows. If it is exceeded, the workload is stopped and the
measurement fails without writing a partial CSV. Metric collection itself may
perform I/O and, depending on the backend, allocate memory. At exactly 1000
samples per second, the capacity covers approximately 4 minutes and 22 seconds.

```bash
sudo ./main --overtime \
  -command "<command>" \
  -language <lang> \
  -program <prog> \
  [-interval-us <microseconds>] \
  [-powercap <W>] \
  [-variance <V>] \
  [-sleep-secs <sec>] \
  [-output <path>]
```

#### Parameters:
* `-command`: Foreground benchmark workload.
* `-language`: Programming language identifier.
* `-program`: Program identifier.
* `-interval-us`: Positive pause between sampling cycles in microseconds (default: `1000`).
* `-powercap`: Clamping limit in Watts (optional, defaults to `-1`).
* `-variance`: Maximum allowed temperature variance above baseline.
* `-sleep-secs`: Stabilization sleep duration.
* `-output`: Output CSV file path (default: `measurements_ot.csv`).

For scripts to analyze observed sample intervals, probe RAPL update behavior,
and compare workload timing overhead, see [scripts/README.md](scripts/README.md#evaluating-overtime-sampling).

---

### 4. Powercap Curve Exploration (`--powercap-calibration`)
Sweeps a power range ($W_{\min} \dots W_{\max}$) to evaluate powercap impacts.

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

Runs tests at each Watt step in the defined range. Results are saved to `powercap_calibration.csv` and summarized automatically if post-processing scripts are present. Since no uncapped baseline is included, baseline-relative GPS-UP metrics cannot be calculated.

On Intel, each requested Watt value is applied to every detected package and die. Destroying the RAPL context does not restore the previous package limits; the configured limits may remain in effect after a measurement or calibration run. The standard and overtime `-powercap` options accept only `-1` (do not set a GreenMeasurer cap) or a positive Watt value.

In standard mode, `cpu.Package_J` is total package energy over the run, not average power. A lower power cap can extend `Time_ms` enough that total energy stays similar or increases; compare both metrics, or derive average package power as `cpu.Package_J / (Time_ms / 1000)`. Do not assume package energy must decrease monotonically with the cap.

---

## Output CSV Schemas

The CSV schema is generated at runtime from the selected CPU and GPU backends. The
framework metadata columns are followed by one column for every metric declared
by those backends. Metric columns use the form
`<category>.<metric-name>_<unit>`; therefore the exact schema varies by machine.
Only selected backends contribute columns.

For example, standard mode on a system with Intel CPU and NVIDIA GPU backends may
produce:

```csv
Language,Program,Powercap,Time_ms,CPU_Memory_KB,Cores,Workload_Processes_Peak,System_Running_Processes_Peak,Core0_Avg_Freq_MHz,...,cpu.Package_J,cpu.Core_J,cpu.DRAM_J,cpu.Temperature_C,gpu.GPU_J,gpu.Power_W,gpu.Temperature_C,gpu.Utilization_%,gpu.VRAM_MiB
```

`Language`, `Program`, and `Powercap` describe the run. `Time_ms` is elapsed
wall-clock time in milliseconds, and `CPU_Memory_KB` is maximum RSS in KiB estimated by
a separate execution measured with `wait4()`/`ru_maxrss`. Backend metric values use the unit in the
column name. `Cores` is the number of online logical cores, `Workload_Processes_Peak` and
`System_Running_Processes_Peak` are the maxima seen during the run, and `Core<N>_Avg_Freq_MHz` is each
core's mean frequency over the run (all sampled every 50 ms). A metric without a supported reading is written as `NOT_SUPPORTED`.

Metric behavior is declared by each backend:

* **Window metrics** are measured between `sample_start` and `sample_stop`.
* **Gauge metrics** are read at the end of a standard run.
* **Peak metrics** are polled during standard runs; process-scoped metrics are
  aggregated across the workload process tree. Polling is every 50 ms, so brief
  allocations may be missed.

In over-time mode `Timestamp_ms` is the monotonic elapsed time in milliseconds
at the start of each sample's backend readings, and
`CPU_Memory_KB` is the current resident set size of the workload process tree.
It also reports `Cores` (online logical cores), `Workload_Processes` (live
processes in the workload tree), `System_Running_Processes` (runnable processes
system-wide) and one `Core<N>_Freq_MHz` column per core. Each of the memory, core-count, process-count, running-process and core-frequency
metrics is read sequentially in each cycle. Values in the same row are therefore
not exactly simultaneous. Process-scoped readings may be unavailable in the
final row because the workload has already exited. For example:

```csv
Language,Program,Powercap,Timestamp_ms,CPU_Memory_KB,Cores,Workload_Processes,System_Running_Processes,Core0_Freq_MHz,...,cpu.Package_J,cpu.Core_J,cpu.DRAM_J,cpu.Temperature_C,gpu.GPU_J,gpu.Power_W,gpu.Temperature_C,gpu.Utilization_%,gpu.VRAM_MiB
```

Window metrics such as energy are **joules consumed in the interval since the
preceding backend reading**, not instantaneous joules or cumulative energy.
Gauges and process-scoped values are readings taken in that cycle. Built-in
backends use consecutive counter snapshots without gaps; legacy plugins without
the optional `energy_backend_sample_next` symbol use separate start/stop windows
and issue a warning because energy between windows is missed. Intel RAPL deltas
handle one 32-bit wrap per interval, but cannot detect multiple wraps. Choose an
interval short enough for the hardware and workload. The timestamp is taken
immediately before the sequential backend reads, so timing of individual metrics
and interval-average power are approximate.
Standard mode continues to accumulate 50 ms segments. Use
`main --csv-header` or `main --overtime-csv-header` to print the schema selected
on the current machine.

---

## Adding a New Architecture / Driver

Each backend owns its metric descriptors and the generic `get_metric` callback.
Adding a metric that fits the existing sampling kinds and scopes requires a
backend change only; the core discovers its name, unit, value and CSV column
dynamically. The HAL enables hardware support extensions without modifying
`main.c`:

1. **Implement the shared backend interface**: CPU drivers implement `CpuBackend`, and GPU drivers implement `GpuBackend` from `include/energy-backend.h`. Place each implementation in its own directory below `platform/linux/cpu/` or `platform/linux/gpu/`:
   ```c
   #include "energy-backend.h"

   static int custom_detect(void) { return 1; }
   static int custom_init(int core) { (void)core; return 0; }
   static int custom_sample_start(int core) { (void)core; return 0; }
   static int custom_sample_stop(int core) { (void)core; return 0; }
   static const EnergyMetricDescriptor custom_metrics[] = {
       {"Package", "J", ENERGY_METRIC_WINDOW, ENERGY_METRIC_DEVICE,
        ENERGY_METRIC_FLAG_NONE}
   };
   static double custom_get_metric(size_t index, int core, pid_t process_id) {
       (void)core;
       (void)process_id;
       if (index != 0) return METRIC_NOT_SUPPORTED;
       return METRIC_NOT_SUPPORTED; /* Replace with the hardware-specific read. */
   }

   CpuBackend custom_cpu_backend = {
       .name = "Custom CPU",
       .detect = custom_detect,
       .init = custom_init,
       .sample_start = custom_sample_start,
       .sample_stop = custom_sample_stop,
       .metrics = custom_metrics,
       .metric_count = sizeof(custom_metrics) / sizeof(custom_metrics[0]),
       .get_metric = custom_get_metric
   };

   const EnergyBackendDescriptor energy_backend_descriptor = {
       .type = ENERGY_BACKEND_CPU,
       .priority = 0,
       .backend.cpu = &custom_cpu_backend
   };
   ```

   GPU implementations use the same lifecycle, implement `GpuBackend`, and set the descriptor type to `ENERGY_BACKEND_GPU`. A higher `priority` selects a preferred implementation when multiple implementations are detected.

2. **Add build metadata**: Add a `backend.mk` file to the implementation directory. The source is discovered automatically when it is the only `.c` file other than `sensors.c`; metadata can also specify the source and any compiler or linker flags:
   ```make
   BACKEND_SOURCE_platform_linux_cpu_custom := custom.c
   BACKEND_CFLAGS_platform_linux_cpu_custom := -I/path/to/headers
   BACKEND_LDLIBS_platform_linux_cpu_custom := -lcustom
   BACKEND_HOST_LDLIBS_platform_linux_cpu_custom := -lcustom-host-support
   ```

   `BACKEND_LDLIBS` links dependencies into the plugin. Use `BACKEND_HOST_LDLIBS` only when the plugin needs symbols provided by the executable.

   The build creates `backend.so` in each implementation directory. At runtime, the executable scans these directories and loads valid plugins; adding an implementation does not require changes to `main.c` or a vendor registry.

   **Backend lookup order:** `$GREENMEASURER_BACKEND_DIR` (a directory containing `cpu/` and `gpu/`), then `<executable dir>/platform/linux`, then `<executable dir>/../lib/greenmeasurer/platform/linux`.

   **Diagnostics:** By default only selected backends are listed; backends that do not apply to the machine (`available` but lower priority, `not detected`, `not built`) are hidden unless their category has no selected backend. Set `GREENMEASURER_VERBOSE_BACKENDS=1` to list every backend, and `make VERBOSE=1` to see skipped-build reasons. At startup the executable prints a table with one row per backend folder and a status: `selected`, `available` (detected but lower priority), `not detected` (with the hint returned by the optional `diagnose()` callback), `not built` (no `backend.so`), `load failed` or `invalid` (bad descriptor or missing callbacks). Missing CPU/GPU backends produce a warning and the corresponding metrics are reported as `NOT_SUPPORTED`.

   **Tests:** `make test` (in `src/`) builds fake plugins in a temporary directory and checks priority selection, undetected/invalid/unbuilt plugins, missing backends and the directory override.