# GreenMeasurer: Design, Implementation and Validation of a Vendor-Agnostic Energy Measurement Framework

> **Historical interface notice:** CLI examples in this document predate the
> TOML-only interface and must not be used. For current generation, execution,
> batch and calibration instructions, see the [README](../README.md) and
> [scripts guide](../scripts/README.md).

Current selection semantics: `metrics.select = "all"` selects all system
metrics, all metrics of the CPU and GPU backends selected at runtime, all
metrics in declared adapters, and all declared source-based external metrics.
Availability checks and failure handling are the same as for explicit selection.

This document describes the architecture and engineering decisions of GreenMeasurer in enough detail to serve as the technical basis of a scientific paper. It covers the design goals, the plugin-based backend API, measurement semantics, build system, diagnostics, testing, limitations and threats to validity.

---

## 1. Motivation and Problem Statement

Measuring the energy consumption of software on commodity hardware relies on vendor-specific interfaces:

| Domain | Typical interface |
|---|---|
| Intel CPU | RAPL counters read through Model-Specific Registers (MSRs) or powercap |
| AMD Zen CPU | RAPL-like MSRs with different register addresses and units |
| Intel integrated GPU | RAPL PP1 domain (MSR) |
| NVIDIA GPU | NVML total-energy counter |

Tools that hard-code these interfaces in their core are difficult to extend, and vendor assumptions leak into user-facing messages and control flow (for example, assuming that "the GPU" is either Intel or NVIDIA). The goal of this work was to restructure GreenMeasurer so that:

1. The orchestrator (`main.c`) is written **once against a vendor-neutral API** and is never modified to add hardware support.
2. Each vendor implementation is an **independent plugin** implementing the same API.
3. The orchestrator **discovers plugins at runtime** from the directory tree instead of using a hard-coded registry.
4. Missing hardware, missing permissions and missing build dependencies **degrade gracefully** and are reported in a clear, vendor-neutral way.
5. Unsupported metrics are **never fabricated** (e.g. a CPU temperature is never reported as a GPU temperature).

### Design requirements (summary)

| ID | Requirement | Realization |
|---|---|---|
| R1 | `main.c` implements only the API and is never changed per vendor | Descriptor-based plugin loading via `dlopen`/`dlsym` |
| R2 | `main.c` contains no vendor names | Verified by `grep -i "intel\|amd\|nvidia" src/main.c` returning nothing |
| R3 | Vendors are read from the `src` folder layout | `scandir` over `platform/linux/{cpu,gpu}/<vendor>/backend.so` |
| R4 | Code, comments and messages in English | Applied to all sources, scripts and messages |
| R5 | Graceful degradation | Explicit validity flags, `METRIC_NOT_SUPPORTED`, readiness checks |
| R6 | Reproducible validation | Fake-plugin test suite (`make -C src test`) |

---

## 2. System Overview

Editable high-level folder map and extension points: [GreenMeasurer architecture diagram](./architecture.drawio).

```
                    +-----------------------------+
                    |           main.c            |
                    |  CLI, measurement loop,     |
                    |  CSV output, calibration    |
                    +--------------+--------------+
                                   | energy-backend.h (shared API)
                 dlopen + dlsym("energy_backend_descriptor")
        +--------------+-----------+-----------+--------------+
        |              |                       |              |
 cpu/<vendor A>   cpu/<vendor B>         gpu/<vendor C>  gpu/<vendor D>
  backend.so       backend.so             backend.so      backend.so
```

Directory convention (`src/`):

```
platform/linux/
  memory.c                     Linux workload peak RSS via wait4()/ru_maxrss
  system_metrics.c             Linux implementation of the shared system-metrics API
  cpu/<implementation>/{<source>.c, <header>.h, backend.so}
  gpu/<implementation>/{<source>.c, <header>.h, backend.so}
include/energy-backend.h     shared API
include/system_metrics.h     system-metrics interface
metric_adapters/             external metric adapter loader and validator
tests/                       fake backends and test runner
```

The category (`cpu` or `gpu`) determines which interface the plugin must implement; the folder name is only an identifier and carries no semantics for the orchestrator.

---

## 3. The Backend API

Defined in `src/include/energy-backend.h`. Two vtable-style structs, `CpuBackend` and `GpuBackend`, plus a descriptor each plugin exports.

The built-in backends use an `EnergyMetricDefinition` array and
`ENERGY_DEFINE_METRIC_TABLE` to prepare their descriptors, function map,
indexed callback and description/origin metadata from one list. The older
`ENERGY_DEFINE_METRICS` interface remains available for existing plugins.
To add a backend, start with the
[CPU or GPU template](../src/backend_templates/) and follow the concise
[backend guide](../README.md#adding-a-backend). The public
`./src/main --list-metrics [cpu|gpu [directory]]` command lists compiled plugins
without detecting hardware or running workloads. Loading a plugin may execute
its native constructors, so inspect only trusted libraries.

### 3.1 CPU interface

| Callback | Required | Purpose |
|---|---|---|
| `name` | yes | Human-readable backend name |
| `detect()` | yes | Returns 1 if the backend can operate on this machine |
| `init(core)` / `cleanup()` | init: yes | Acquire / release resources |
| `sample_start(core)` / `sample_stop(core)` | yes | Counter snapshots delimiting a measurement window |
| `metrics`, `metric_count` | yes | Backend-owned metric descriptors and their count |
| `get_metric(index, core, process_id)` | yes | Returns the indexed metric value or `METRIC_NOT_SUPPORTED` (-1.0) |
| `set_powercap(W)`, `destroy_powercap()` | optional | Power limiting |
| `calibrate_powercap(...)` | optional | Powercap curve exploration |
| `diagnose()` | optional | Short hint on why `detect()` failed |

Plugins can also export the optional `energy_backend_sample_next(int id)` symbol.
After `sample_start`, each call closes the current window, exposes its metric
values through `get_metric`, and uses that stop snapshot as the next baseline.
This avoids gaps between successive over-time energy windows without changing the
backend descriptor ABI. Legacy plugins still work, but the core warns that their
separate stop/start windows omit energy between windows.

### 3.2 GPU interface

| Callback | Required |
|---|---|
| `name`, `detect`, `init`, `sample_start`, `sample_stop`, `metrics`, `metric_count`, `get_metric` | yes |
| `cleanup`, power-capping callbacks for CPU, `diagnose` | optional |

Each `EnergyMetricDescriptor` defines a stable name, unit, sampling kind
(`WINDOW`, `GAUGE`, or `PEAK`), scope (`DEVICE` or `PROCESS`) and optional
semantic flags such as CPU or GPU temperature. The selected backends provide
the metric list; `main.c` validates it and constructs the output schema as
`<category>.<name>_<unit>`. Adding ordinary telemetry therefore does not require
changing the core API or `main.c`; new sampling semantics would require extending
the shared API deliberately.

### 3.3 Plugin descriptor

Each plugin exports exactly one symbol, `energy_backend_descriptor`:

```c
typedef struct energy_backend_descriptor {
    EnergyBackendType type;     /* ENERGY_BACKEND_CPU or ENERGY_BACKEND_GPU */
    int priority;               /* higher wins among detected backends */
    union { CpuBackend *cpu; GpuBackend *gpu; } backend;
} EnergyBackendDescriptor;
```

The descriptor replaces the earlier approach of weak-symbol `extern` declarations in the core, which required `main.c` to know every vendor symbol at link time.

### 3.4 Capability model

Optional callbacks are expressed as `NULL` function pointers. The core checks
them before use (e.g. powercap calls are skipped, with a clear error, when no
CPU backend or no `set_powercap` exists). A backend declares its own metrics;
unavailable telemetry returns `METRIC_NOT_SUPPORTED`, serialized as
`NOT_SUPPORTED` in the corresponding dynamic CSV column. This avoids silently
mixing "zero energy" with "not measurable".

---

## 4. Runtime Plugin Discovery

Implemented in `main.c` (`get_backend_root`, `load_backend_category`, `load_backend_plugin`, `init_backends`).

1. **Resolve the backend root**, in order:
   1. `$GREENMEASURER_BACKEND_DIR` (must contain `cpu/` and `gpu/`),
   2. `<executable dir>/platform/linux`,
   3. `<executable dir>/../lib/greenmeasurer/platform/linux`.
   The executable path is obtained from `/proc/self/exe`, so symlinked invocations resolve correctly.
2. **Scan** `cpu/` and `gpu/` with `scandir`; every sub-directory is a candidate implementation.
3. **Load** `<implementation>/backend.so` with `dlopen(RTLD_NOW | RTLD_LOCAL)` and look up the descriptor with `dlsym`.
4. **Validate** the descriptor: type matches the category, required callbacks and metric descriptors are valid, and metric keys are unique within the backend. Invalid plugins are rejected and reported, never executed.
5. **Select**: for each category, among plugins whose `detect()` returns 1, choose the one with the highest `priority`. (Dedicated hardware, such as a discrete GPU with a direct energy counter, can thus override a generic implementation by declaring a higher priority.)
6. **Clean up** at exit: active backends' `cleanup()`, then `dlclose` of all handles.

Adding hardware support therefore consists of creating a new folder with a
source file, backend-owned metric descriptors, a generic getter and the
descriptor export. No change to `main.c`, the Makefile logic or any registry is
required.

---

## 5. Build System

`src/Makefile` discovers `platform/linux/*/*` directories and builds one `backend.so` per directory using `-fPIC -shared`. The core is linked with `-rdynamic -ldl`.

Backend-specific build settings live in `src/Makefile`, using the naming convention `BACKEND_<KEY>_<path with / replaced by _>`:

| Variable | Meaning |
|---|---|
| `BACKEND_SOURCE_*` | Source file (default: first `*.c` not named `sensors.c`) |
| `BACKEND_EXTRA_SOURCES_*` | Additional source files linked into the plugin |
| `BACKEND_AVAILABLE_*` | `yes`/`no`; `no` skips the plugin and prints a `[BUILD] ... skipped` line |
| `BACKEND_CFLAGS_*` | Extra compile flags/include paths |
| `BACKEND_LDLIBS_*` | Libraries linked into the plugin |
| `BACKEND_HOST_LDLIBS_*` | Libraries linked into the main executable |

`BACKEND_HOST_LDLIBS` exists for a practical reason: some dependencies (e.g. the Intel RAPL powercap libraries on the test machine) are installed only as non-PIC static archives, which cannot be linked into a shared object. They are linked into the executable with `--whole-archive`, and exposed to plugins through `-rdynamic`.

Optional dependencies are checked per backend at build time. For example, the Intel CPU plugin is skipped unless both powercap/raplcap headers and libraries are present, and the NVIDIA plugin is skipped unless both the NVML header and library are present. A missing optional dependency therefore does not prevent the core or other eligible plugins from building. Plugins that build still perform hardware detection at runtime, so only compatible hardware is selected.

---

## 6. Measurement Semantics and Correctness Fixes

The following defects were found during the architectural review and fixed.

### 6.1 Counter rollover
RAPL energy counters are 32-bit and wrap around. Deltas are computed with modular unsigned arithmetic:

```
energy = (uint32_t)(after - before) * energy_unit
```

This is correct across a single wrap. It cannot detect two or more wraps within one window; long windows should use over-time sampling (Section 6.5).

### 6.2 Validity flags
Each backend tracks whether the "before" and "after" samples were read successfully (`*_valid` flags, `energy_units_valid`). If any is invalid, the getter returns `METRIC_NOT_SUPPORTED` rather than a bogus difference computed from uninitialised or `-1` register values.

### 6.3 Error handling
- `init`, `sample_start`, `sample_stop` return codes are checked by the core (`cpu_sample_ready`/`gpu_sample_ready`); a failed sample marks the corresponding metrics as unsupported.
- The AMD backend no longer calls `exit(127)` on MSR open failure; it returns an error so the core can continue with other backends.
- The core null-checks the active CPU backend before powercap operations. When a CPU
  backend is active, measurements require a readable CPU temperature, even when only GPU metrics
  are selected. Without a CPU backend but with a GPU backend, thermal control is skipped.
- Permission and device errors are reported once instead of on every sample.

### 6.4 Temperature semantics
Previously, when no GPU sensor existed, the CPU temperature was copied into the GPU temperature column. This produced misleading data (a "GPU temperature" that was not measured). The fallback was removed:

- `Temperature` is a direct sensor reading or `NOT_SUPPORTED`.
- The Intel CPU backend uses the lm-sensors C library to average readable
  `Core` temperature inputs from the `coretemp` chip.
- The Intel integrated GPU backend reads hwmon sensors (`/sys/class/hwmon/*/temp*_label` whose label starts with `GPU`). The CPU package temperature is deliberately **not** used even though the iGPU shares the package, because it is a package-level value and not a GPU sensor.
- `detect()` for the integrated GPU backend returns 1 if either the PP1 energy counter or a GPU temperature sensor is accessible.

### 6.5 Memory semantics
The standard CSV reports `CPU_Memory_KB`, the maximum resident set size in KiB
reported by `wait4()` (`ru_maxrss`) for each measured run. This value is not a
sum of simultaneous RSS across the workload's process tree.

A backend may declare a process-scoped metric such as `VRAM` with unit `MiB`.
The NVIDIA backend queries NVML process lists for the workload process and its
descendants and sums their allocated memory. The standard mode polls peak
metrics every 50 ms during execution; over-time mode records the observed
process-scoped value at each sample. Very short-lived allocations may be
missed. If the driver, permissions or GPU backend cannot provide per-process
data, the value is `NOT_SUPPORTED`. This process-level approach avoids
attributing allocations belonging to unrelated jobs on a shared accelerator
to the measured workload. Integrated GPUs that share system RAM do not report
separate VRAM through the current backend.

### 6.6 Dynamic CSV schema

The standard CSV starts with `Language,Program,Powercap,Time_ms,CPU_Memory_KB`;
over-time uses `Timestamp_ms` instead, the monotonic elapsed time at the start
of each sample's backend readings. These common
metadata fields are followed by columns declared by the selected plugins, in
CPU-then-GPU order. A metric column is named
`<category>.<metric-name>_<unit>`, for example `cpu.Package_J` or
`gpu.VRAM_MiB`. Thus, machines with different selected backends can have
different headers. The `--csv-header` and `--overtime-csv-header` modes print
the current runtime schema, and the measurement wrapper uses that header when
aggregating output from multiple workloads.

### 6.7 Over-time sampling
`--overtime` performs over-time sampling, recording each window metric's delta since the previous backend
snapshot, rather than accumulated energy. The parent samples all metrics
sequentially without sampler threads, using a configurable pause (`-interval-us`,
default 1000 microseconds); a final partial window is sampled on workload
completion. Samples are buffered and dumped after completion. Times vary with
metric overhead and scheduling; the interval does not synchronize polling with
hardware updates. Intel counter
deltas handle one 32-bit wrap between readings, but cannot detect multiple wraps.
Gauge and process-scoped metrics are recorded at each tick. Standard-mode metric behavior is declared by each backend: window metrics
use start/stop samples, gauges are read after the workload, and peak metrics are
polled during execution.

### 6.8 NVIDIA detection
Detection is based on NVML initialisation and device count (`nvmlInit`, `nvmlDeviceGetCount`), consistent between the build step (header and library presence) and runtime.

---

## 7. Hardware Coverage

| Backend | Category | Energy source | Powercap | Temperature |
|---|---|---|---|---|
| Intel RAPL CPU | cpu | MSR (package, cores, DRAM) | yes (raplcap/powercap) | CPU sensors |
| Intel Integrated GPU | gpu | MSR PP1 | n/a | hwmon `GPU` label, if present |
| NVIDIA NVML GPU | gpu | NVML total energy (priority 100) | n/a | NVML |

Only the Intel CPU backend implements power capping, and the GPU interface has no powercap callbacks. AMD and macOS backends are not currently provided.

Known hardware constraints: MSR access requires root and the `msr` kernel module (`/dev/cpu/N/msr`).

---

## 8. Diagnostics and User Experience

A vendor-neutral table is printed at startup. By default it shows selected
backends and plugin load or descriptor errors; other entries are hidden.
Set `GREENMEASURER_VERBOSE_BACKENDS=1` to see every backend folder, for example:

```
[INFO] Energy backends (<root>)
  TYPE  BACKEND               STATUS
  cpu   Intel RAPL CPU        selected
  gpu   Intel Integrated GPU  selected
  gpu   nvidia                not built
```

Intel GPU hardware is detected from PCI/sysfs independently of MSR permissions.
The GPU backend can therefore be selected without root even when its RAPL energy
counter cannot be read; in that case GPU energy metrics are unavailable until
the process has MSR access.

| Status | Meaning |
|---|---|
| `selected` | Detected and highest priority in its category |
| `available` | Detected but outranked |
| `not detected` | `detect()` returned 0; details come from the optional `diagnose()` callback |
| `not built` | Folder present without `backend.so` |
| `load failed` | `dlopen` error (message from `dlerror`) |
| `invalid` | Missing descriptor symbol, wrong type, or missing required callbacks |

When a category has no active backend a warning states that its backend metrics
are unavailable and points to verbose diagnostics, which may explain a
permission failure. The legacy runner exits if neither category is active;
configured system/external over-time measurements can run without either. Wrapper scripts
(`scripts/measure.sh`) and Makefile messages were also made vendor-neutral.
Vendor-specific logic remains only where it is inherently vendor-specific
(the dependency installer `scripts/install.sh`, backend sources and build settings in `src/Makefile`).

---

## 9. Testing and Validation

### 9.1 Automated tests (`make test` in `src/`)
`tests/fake_backend.c` is compiled with configurable name, priority, detection result, category and validity into several `backend.so` files placed in a temporary tree. `tests/run_tests.sh` points the real binary at that tree through `GREENMEASURER_BACKEND_DIR` and checks:

| Scenario | Expected behaviour |
|---|---|
| Two detected CPU plugins with different priority | Higher priority is `selected`, other is `available` |
| Plugin whose `detect()` returns 0 | `not detected` with the `diagnose()` hint |
| Plugin missing required callbacks | `invalid` |
| Folder without `backend.so` | `not built` |
| Valid GPU plugin | `selected` |
| No GPU plugins | Warning, run continues, exit code 0 |
| No plugins at all | Explicit error, non-zero exit |
| Bad override directory | Explicit error |

Result: 13 assertions passed, 0 failed.

### 9.2 Hardware validation
On a Lenovo ThinkPad T14 Gen 4 (Intel CPU with integrated GPU, no NVIDIA GPU):
- With or without root: the Intel CPU and Intel integrated GPU backends are detected and selected.
- Without root: MSR access is denied, so energy metrics degrade to `NOT_SUPPORTED`.
- The hwmon "GPU" label was not populated on this laptop, so `Temperature` is `NOT_SUPPORTED`, as intended.

### 9.3 Static invariants
- `main.c` contains no vendor identifiers.
- `git diff --check` reports no whitespace errors.

---

## 10. Limitations and Threats to Validity

1. **Not validated here:** the NVIDIA backend could not be compiled in the development environment (no `nvml.h`); it follows the same API and was reviewed, but was not executed.
2. **Privilege requirements:** MSR-based energy measurement needs root and the `msr` module. Non-root runs produce `NOT_SUPPORTED`, not zero.
3. **Counter semantics:** package energy includes components beyond the measured process (OS, background tasks). The framework mitigates noise with temperature-baseline calibration, repeated runs (`-n_times`), a temperature variance threshold and optional Wi-Fi disablement, but cannot attribute energy per process.
4. **iGPU/CPU coupling:** PP1 (integrated GPU) energy may be part of package-domain accounting; summing Package and GPU values could double count in some configurations.
5. **Rollover:** a single 32-bit wrap is handled per window; multiple wraps in one window are not detectable.
6. **Plugin trust:** `dlopen` runs arbitrary code from the backend directory. The `GREENMEASURER_BACKEND_DIR` override must point to a trusted location, particularly when running as root.
7. **Platform scope:** only Linux backends exist. Other operating systems are rejected at build time.
8. **Plugin ABI:** the API is an in-tree C ABI with no version field; plugins must be rebuilt together with the core.
9. **Sensor availability:** temperature depends on kernel hwmon exposure that varies by vendor and model.

---

## 11. Future Work

- Add an ABI/API version field to the descriptor and reject incompatible plugins.
- Per-process attribution (e.g. cgroup-based accounting) to complement package-level counters.
- Additional backends (e.g. AMD GPU via ROCm SMI, other operating systems) via the existing plugin mechanism.
- Install target (`make install`) using the `../lib/greenmeasurer/` lookup path.
- Integration tests with recorded counter traces to validate rollover and unit conversion on real register values.
- Quantitative evaluation of measurement overhead and of cross-backend agreement for the paper.

---

## 12. Suggested Paper Structure

1. **Introduction**: heterogeneity of energy interfaces and the cost of vendor-coupled tools.
2. **Background**: RAPL, AMD energy MSRs, NVML, hwmon.
3. **Design**: requirements R1–R6, the API (Section 3), the descriptor and capability model.
4. **Implementation**: discovery (Section 4), build integration (Section 5), correctness measures (Section 6).
5. **Evaluation**: functional tests (9.1), hardware case study (9.2); to add: overhead, accuracy versus an external power meter, extensibility effort (lines of code to add a new backend).
6. **Discussion**: limitations (Section 10).
7. **Related work**: tools such as PowerAPI, Scaphandre, RAPL-based tools, CodeCarbon and pyJoules.
8. **Conclusion and future work**.

### Reproducibility checklist
- Build: `cd src && make`
- Tests: `cd src && make test`
- Run: `sudo bash scripts/measure.sh --standard all`
- Backend override: `GREENMEASURER_BACKEND_DIR=/path/to/platform/linux ./main ...`
- Record: CPU model, kernel version, driver versions, whether run as root, and the backend table printed at startup.
