# GreenMeasurer: An Aspect-Oriented Framework for Non-Intrusive Software Energy Measurement

<div align="center">
  <img src="logo.svg" alt="GreenMeasurer" height="70" valign="middle">&nbsp;&nbsp;
  <a href="https://www.greenmeasurer.wasmer.app"><img src="https://img.shields.io/badge/Website-greenmeasurer.wasmer.app-2ea44f?style=flat-square" alt="Website" valign="middle"></a>
  <img src="https://img.shields.io/badge/Platform-Linux-blue?style=flat-square" alt="Platform" valign="middle">
</div>

**GreenMeasurer** is an open-source, language-agnostic, and extensible framework for non-intrusive measurement of software energy consumption and runtime metrics.

Following an **Aspect-Oriented Programming (AOP)** approach, hardware and system metrics are declared in human-readable **TOML** aspect files under `[measurement]`, `[device]`, and `[[metric]]` tables. A weaver compiles and links the available aspects with a lightweight C runtime to produce a standalone Linux measurement executable, requiring **zero changes** to the target program's source code.

### Authors

| Author | Affiliation | Contact |
| :--- | :--- | :--- |
| **Simão Cunha** | INESC TEC & University of Minho | [simao.s.cunha@inesctec.pt](mailto:simao.s.cunha@inesctec.pt) |
| **João Saraiva** | INESC TEC & University of Minho | [saraiva@di.uminho.pt](mailto:saraiva@di.uminho.pt) |

---

## Requirements
You can install the required system packages and libraries automatically using the provided installation script:

```sh
bash scripts/install.sh
```

Alternatively, ensure the following dependencies are available on your system:
- **Python 3.11+** (or Python 3 with `tomli`, plus `pandas` for power-cap calibration post-processing)
- **C compiler** (`gcc`/`clang`), `make`, and **POSIX threads** (`pthread`)
- **Hardware & library dependencies** for the aspects you choose to build:
  - *Intel CPU RAPL energy:* Linux MSR access (`/dev/cpu/*/msr`) — no extra library dependencies
  - *Intel CPU temperature:* `lm-sensors` development files (`libsensors`)
  - *Intel CPU power capping:* `powercap` and `raplcap` development files
  - *NVIDIA GPU metrics:* NVML headers and runtime library

Build the device libraries whose dependencies are installed on your system:

```sh
make -C src
```

> **Note:** Missing optional headers do not break the build—the affected device library is skipped automatically during `make -C src`.

---

## Quick Start: Generate & Run

### 1. Weave the Measurement Executables

Each metric aspect specifies a monitoring `mode` (`aggregate` or `over-time`), while control aspects (such as `IntelCPU.Powercap`) are mode-independent and can be woven into both executables:

```sh
# Build the aggregate executable (one summary row per execution)
python3 scripts/weave.py \
  available-aspects/aggregate/intel-cpu.toml \
  available-aspects/aggregate/intel-cpu-temperature.toml \
  available-aspects/aggregate/intel-cpu-powercap.toml \
  available-aspects/aggregate/system.toml \
  available-aspects/aggregate/time.toml \
  -o generated-aggregate

# Build the over-time executable (periodic samples on a fixed millisecond grid)
python3 scripts/weave.py \
  available-aspects/over-time/intel-cpu.toml \
  available-aspects/over-time/intel-cpu-temperature.toml \
  available-aspects/over-time/intel-cpu-powercap.toml \
  available-aspects/over-time/system.toml \
  available-aspects/over-time/time.toml \
  -o generated-overtime
```

Generation does not run the workload. If an aspect cannot be compiled or linked, the weaver logs a warning and omits its flags. Inspect the incorporated metrics and hooks with:

```sh
./generated-aggregate/greenmeasurer --help
```

### 2. Calibrate Baseline Temperature & Power Cap *(Optional)*

Before running energy benchmarks, record an idle temperature baseline and sweep candidate CPU power limits to identify the most energy-efficient cap:

```sh
# 1) Record idle CPU temperature baseline (stabilizes for 30s before sampling)
sudo ./generated-aggregate/greenmeasurer --temperature-calibration \
  -sleep-secs 30 \
  --IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=cpu-baseline.txt

# 2) Sweep power caps from 5 W to 15 W (plus baseline -1) and save the optimal limit
sudo ./generated-aggregate/greenmeasurer --powercap-calibration \
  -command "python3 paper-experiments/fibonacci.py 40" \
  -program fibonacci-40 \
  -output powercap-sweep.csv \
  -range 5-15 -n_times 5 -sleep-secs 30 -timeout-hours 1 \
  -temperature-control IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=cpu-baseline.txt -variance 5 \
  --IntelCPU.Powercap --IntelCPU.Package --Time.WallClock
```

The post-processing script evaluates **Speedup, Greenup, and Powerup (GPS-UP)** metrics and writes the lowest-energy power limit to `powercap-sweep-optimal-powercap.txt`.

### 3. Run Measurements (`aggregate` & `over-time`)

Execute the target program without modifying its source code, enabling metrics explicitly via CLI flags:

```sh
# Aggregate mode: 10 repetitions per power cap (-1 = uncapped, 5 = 5 W limit)
sudo ./generated-aggregate/greenmeasurer \
  -command "python3 paper-experiments/fibonacci.py 40" \
  -program fibonacci-40 \
  -output aggregate.csv \
  -n_times 10 -sleep-secs 30 -powercap -1,5 -timeout-hours 1 \
  -temperature-control IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=cpu-baseline.txt -variance 5 \
  --IntelCPU.Powercap --IntelCPU.Package --IntelCPU.Temperature \
  --System.RunningProcesses --Time.WallClock

# Over-time mode: periodic samples every 10 ms aligned on Timestamp_ms
sudo ./generated-overtime/greenmeasurer \
  -command "python3 paper-experiments/fibonacci.py 40" \
  -program fibonacci-40 \
  -output overtime.csv \
  -n_times 1 -interval-ms 10 -sleep-secs 30 -powercap -1,5 -timeout-hours 1 \
  -temperature-control IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=cpu-baseline.txt -variance 5 \
  --IntelCPU.Powercap --IntelCPU.Package --IntelCPU.Temperature \
  --System.RunningProcesses --Time.WallClock
```

> 💡 To generate and run both modes in batch from a single command, use `scripts/measure.sh` (see [scripts/](scripts/)).

---

## Runtime Behavior & Conventions

- **Relative Paths:** Output CSV and calibration paths are resolved relative to the executable's working directory, whereas `header` and `lib` paths inside `.toml` files are resolved relative to each `.toml` file.
- **CSV Appending & Validation:** If an output CSV already exists, new rows are appended provided the header matches the selected mode and metrics; mismatched headers are rejected.
- **Explicit Metric Selection:** Metrics must be enabled explicitly via CLI flags (e.g., `--IntelCPU.Package`) or disabled with `--no-metrics`. With no active aspects, `-command <cmd>` (and optional `-n_times <N>`) executes the command directly without writing a CSV.
- **Fault-Tolerant Setup:** If a selected aspect fails `setup()`, the runtime emits a warning and continues without it; if no active aspects remain, the command runs once without measurement. Calibration modes fail fast if their target aspect is unavailable.
- **Decoupled Thermal Control:** `-temperature-control <metric>` pauses between runs until the sensor cools to `baseline + variance`. If a controlled sensor becomes unavailable, its wait loop is disabled with a warning while other controlled sensors remain active.
- **Explicit Execution Time:** Wall-clock duration is declared and selected like any other aspect (`--Time.WallClock`). In `over-time` mode, `Timestamp_ms` is always recorded as the common sampling grid axis.

---

## Repository Structure

```text
GreenMeasurer/
├── README.md
├── available-aspects/   # Ready-to-use TOML aspect declarations (aggregate/ and over-time/)
├── paper-experiments/   # Benchmark setup and scripts reproducing the paper's evaluation
├── scripts/             # Aspect weaver (weave.py), batch runner (measure.sh), installer
└── src/                 # C device libraries, shared headers, and measurement runtime
```

| Directory | Description |
| :--- | :--- |
| [**available-aspects/**](available-aspects/) | Ready-to-use TOML aspect specifications for `aggregate/` and `over-time/` modes covering `[measurement]`, `[device]`, and `[[metric]]` declarations. |
| [**paper-experiments/**](paper-experiments/) | Aspect declarations, runner scripts, and experimental setup reproducing the paper's workflow (temperature & power-cap calibration, aggregate, and over-time runs). |
| [**scripts/**](scripts/) | Core weaver (`weave.py`), batch generation/execution helper (`measure.sh`), and dependency installer (`install.sh`). |
| [**src/**](src/) | C source tree (`src/devices`, `src/include`, `src/weaver`) and build targets (`make -C src`, `make -C src clean`). See [src/README.md](src/README.md). |