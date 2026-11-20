# GreenMeasurer: A Non-Intrusive Framework to Measure the Energy Consumption of Software Systems

> **Historical draft:** This describes the previous engine and its removed
> plugin interface. Its architecture, configuration examples and commands do
> not describe the current implementation. See [WEAVER.md](WEAVER.md) and the
> [README](../README.md) for current usage.

> **Draft status.** This document follows the section structure of `GreenMeasurer.pdf` while documenting only capabilities present in the current repository. It is an engineering and paper-writing draft, not a completed scientific evaluation. Quantitative accuracy, measurement overhead and broad hardware compatibility still require controlled experiments.

## Abstract

GreenMeasurer is a Linux framework for measuring the energy consumption and execution time of software without modifying the target program. It combines a C measurement executable, hardware-specific energy backends, shell scripts and workload Makefiles. The executable provides aggregate repeated measurements and time-series energy sampling, as well as temperature and power-cap calibration workflows. A shared CPU/GPU backend interface and runtime plugin discovery separate measurement orchestration from hardware-specific counter access. The framework records energy, execution time, temperatures and memory-use data in CSV files, while reporting unsupported metrics explicitly rather than substituting unrelated values. This draft contains historical workflow examples with updated mode terminology, not migrated commands; consult the README for current implementation details.

**Index Terms—** software energy measurement, energy efficiency, RAPL, hardware telemetry, power capping, Linux.

## I. Introduction

Energy consumption is an increasingly important software quality attribute. It affects battery life, operating cost and the environmental footprint of computing systems. Measuring software energy is challenging because energy counters are exposed through platform-specific interfaces, may require elevated privileges, and frequently report energy for hardware domains rather than for an individual process.

GreenMeasurer aims to make software-energy experiments repeatable without requiring instrumentation or source-code changes to the target workload. A user supplies a command and experiment parameters; the framework prepares the measurement environment, invokes the target and records available telemetry. Its current Linux implementation includes CPU and GPU energy backends, temperature readings, memory usage, execution timing and optional CPU power capping.

An architectural requirement is that hardware support must not be encoded in the measurement orchestrator. Each implementation provides the common backend API and is loaded as a plugin. The core scans the backend directory tree, validates plugins and selects a detected implementation by priority. This design allows additional hardware implementations to be added without adding vendor-specific branches to `main.c`.

The current work comprises:

1. A shared C API for CPU and GPU energy backends, with runtime plugin discovery.
2. Aggregate and over-time measurement modes, and separate temperature and power-cap calibration modes.
3. Explicit handling of unavailable counters, failed samples and unsupported metrics.
4. A fake-plugin test suite for discovery, validation, priority selection and graceful degradation.
5. Documentation of the current system and the experimental evidence still needed for a scientific evaluation.

The remainder follows the organization of the supplied paper: Section II discusses related approaches; Section III describes the framework; Section IV gives a reproducible demonstration protocol; and Section V concludes with limitations and future work.

## II. Related Work

Software-energy measurement tools differ in the level at which they operate, the hardware counters they use, and whether they require application instrumentation. Hardware interfaces such as Intel RAPL provide energy measurements for platform domains, while tools built around language-specific APIs can expose these measurements from within a program. The supplied paper discusses RAPL and power-capping interfaces [5]–[7], earlier energy-efficiency studies [8]–[10], and software tools including pyRAPL, jRAPL, CodeCarbon, Scaphandre and Kepler [13]–[17].

GreenMeasurer is compared experimentally against two tools: **CodeCarbon** and **EnergiBridge**. CodeCarbon is a Python library that estimates energy and carbon emissions from hardware counters and, when they are unavailable, from models, and is typically integrated into the program under test. EnergiBridge is a cross-platform command-line tool that measures the energy of a command through hardware counters. [TODO: confirm the description, supported platforms and references of both tools against their current documentation before submission.]

GreenMeasurer's positioning relative to them rests on three properties:

1. **Ease of use.** The user supplies a command and a few parameters; no instrumentation or code change is required, and the same workflow applies to any language.
2. **Ease of extension.** Hardware support is added by dropping a self-contained backend directory (CPU or GPU) into the source tree. The core, the Makefile and the CSV schema adapt automatically, with no vendor-specific branch in `main.c`.
3. **Graceful degradation.** CPU and GPU backends are selected independently at runtime. A missing or unsupported backend is reported as `NOT_SUPPORTED` rather than aborting the run or silently substituting an estimate.

Further capabilities are a built-in temperature-based stabilisation step (the run waits until temperatures are close to a calibrated baseline), aggregate and time-series (over-time) modes, and optional CPU power capping, which has been used in prior studies [cite]. All measurements are hardware-domain measurements (package, core, DRAM, GPU), not per-process attributions, and they include activity outside the target command.

Additional advantages, each with its justification:

- **Testable hardware support.** The plugin interface allows fake backends, and `make -C src test` uses them to check priority selection, undetected, invalid and unbuilt backends, the absence of a GPU or of any backend, an invalid backend directory, and the abort on power-cap failure. *Justification:* correct fallback behaviour is verified by tests without needing every hardware combination (these tests check orchestration, not counter accuracy). Tools that read counters directly from their own code cannot be tested this way.
- **Self-describing, always-consistent output.** Each backend declares its metrics (name, unit, kind and scope), and the CSV header is generated from the selected backends. `--csv-header` and `--over-time-csv-header` print the columns for a machine without running a workload. *Justification:* the output cannot drift from what the hardware provides, and a new metric needs no change to the core. A missing reading is `NOT_SUPPORTED`, never zero, so downstream analysis cannot mistake absent data for a measured value.
- **Controlled experimental conditions.** The temperature calibration and variance threshold delay a run until the CPU is near an idle baseline, and the scripts disable Wi-Fi during runs and restore it afterwards. *Justification:* temperature and background network activity are known sources of variation in energy measurements, and handling them in the tool makes experiments easier to repeat. Whether this reduces variance must be shown experimentally (see the evaluation plan).
- **Batch experiments across languages.** Workloads are directories with a Makefile exposing `compile`, `aggregate` and `overtime`, and `scripts/measure.sh` runs them for several languages and collects the results in one CSV. *Justification:* adding a language or program is a new directory, which supports comparing implementations of the same algorithm under identical conditions.
- **Independent CPU and GPU selection, with explicit diagnostics.** At startup the tool reports selected backends and plugin errors, and warns when a CPU or GPU backend is missing. Verbose mode shows every backend's state (`selected`, `available`, `not detected`, `not built`, `load failed`, `invalid`) and its diagnostic hint. *Justification:* users can investigate missing metrics without always seeing irrelevant backends.
- **Small dependency footprint.** The core is C with `dlopen`; optional dependencies (powercap/raplcap, NVML) only affect the backend that needs them, and a missing one skips that backend rather than failing the build. *Justification:* the tool builds on machines that lack some libraries, which supports the portability goal.

The bracketed reference numbers above are retained from the supplied PDF and have not been re-verified.

## III. The GreenMeasurer Framework

### A. Overview

GreenMeasurer is composed of a measurement executable, shared backend API, loadable backend implementations, scripts and workload-specific build/run definitions.

```text
                         +------------------------------+
                         |            main.c            |
                         | CLI, orchestration, sampling,|
                         | calibration and CSV output  |
                         +---------------+--------------+
                                         |
                              energy-backend.h API
                                         |
                   dlopen + dlsym("energy_backend_descriptor")
                     +-------------------+-------------------+
                     |                   |                   |
                 cpu/<name>          cpu/<name>          gpu/<name>
                  backend.so          backend.so          backend.so
```

The current source layout is:

```text
src/
  main.c
  include/energy-backend.h
  platform/linux/
    cpu/<implementation>/
    gpu/<implementation>/
  tests/
  platform/linux/memory.c
scripts/
Languages/                 example workload directories and Makefiles
Utils/                     calibration post-processing utilities
docs/
```

#### Domain model

The following conceptual model describes the framework's domain. It is a model of responsibilities and data, not a claim that each concept is represented by a corresponding C structure.

```text
+-------------------+       configures       +-------------------+
|     Experiment    |----------------------->|   MeasurementRun  |
| command, labels,  |                        | one workload run  |
| mode, thresholds  |                        +---------+---------+
+---------+---------+                                  |
          | 1..*                                       | executes
          |                                            v
          |                                  +-------------------+
          |                                  |     Workload      |
          |                                  | command + inputs  |
          |                                  +-------------------+
          |
          | uses
          v
+-------------------+        selects         +-------------------+
| MeasurementEngine |------------------------| BackendPlugin     |
| orchestration,    |  0..1 CPU + 0..1 GPU   | detect, init,     |
| sampling, output  |                        | sample, read       |
+---------+---------+                        +---------+---------+
          |                                            |
          | produces                                   | observes
          v                                            v
+-------------------+                        +-------------------+
| MeasurementRecord |                        | HardwareDomain   |
| time, memory,      |                        | CPU package/core,|
| temperatures,     |                        | DRAM, GPU, sensor|
| energy metrics     |                        +-------------------+
+---------+---------+
          |
          | contains
          v
+-------------------+       associated with +-------------------+
|      Metric       |------------------------| MeasurementWindow |
| value + unit OR   |                        | start, stop, or   |
| NOT_SUPPORTED     |                        | timestamp         |
+-------------------+                        +-------------------+

+-------------------+       establishes      +-------------------+
|   Calibration     |------------------------| RunConfiguration |
| temperature base- |                        | thresholds and   |
| line / power sweep|                        | optional cap     |
+-------------------+                        +-------------------+
```

An `Experiment` describes the workload and measurement policy; it may produce multiple `MeasurementRun` instances. The `MeasurementEngine` chooses at most one detected plugin per category (CPU and GPU), and each plugin exposes measurements for the hardware domains it supports. A `Metric` is either a value with a defined unit or explicitly unavailable; unavailable data is not represented as zero. A `MeasurementRecord` associates the observed metrics with a run and a time window or timestamp. RAM is represented as workload maximum RSS, while VRAM is process-tree GPU allocation when the backend supports it. Temperature calibration supplies a baseline and threshold configuration, while power-cap calibration evaluates a sequence of run configurations.

The folder category determines whether a plugin must implement the CPU or GPU interface. The directory's name is not interpreted by the orchestrator as a hardware vendor name. At startup, the core scans candidate directories, loads `backend.so`, validates the exported descriptor and required callbacks, calls `detect()`, then selects the highest-priority detected plugin in each category. It can select one CPU backend and one GPU backend.

The backend root is chosen in this order:

1. The directory named by `GREENMEASURER_BACKEND_DIR`, which must contain `cpu/` and `gpu/`.
2. `platform/linux` next to the executable.
3. `../lib/greenmeasurer/platform/linux` relative to the executable directory.

The backend API is declared in `src/include/energy-backend.h`. A plugin exports `energy_backend_descriptor`, containing its category, priority and a pointer to a `CpuBackend` or `GpuBackend` structure. Required operations cover detection, initialization, sampling-window start/stop and generic metric retrieval. Each backend declares its own metric descriptors, including a stable key, unit, sampling kind, scope and optional semantic flags. The core dynamically combines descriptors from the selected CPU and GPU plugins and generates the CSV schema; ordinary new metrics do not require changes to `main.c` or the shared API. Optional operations include cleanup, power capping and a diagnostic hint. An unavailable measurement is represented by `METRIC_NOT_SUPPORTED` and written to CSV as `NOT_SUPPORTED`.

The current backend implementations are:

| Category | Implementation | Energy source | Additional capabilities |
|---|---|---|---|
| CPU | Intel | RAPL MSRs | CPU energy domains, temperature integration and power capping |
| GPU | Intel integrated GPU | RAPL PP1 MSR when available | Direct GPU hwmon temperature when exposed |
| GPU | NVIDIA | NVML total-energy counter | NVML telemetry; optional plugin depending on build-time dependencies |

These are the implementations currently present in the repository, not a promise that every model exposes every counter. For example, MSR access requires the `msr` kernel module and sufficient privileges. GPU temperature is never inferred from CPU/package temperature.

### B. System Requirements

The implementation targets Linux. The build requires a C compiler, GNU Make and the dynamic-loader library. Specific plugins may need additional libraries or headers. The Intel CPU power-capping backend uses the configured `raplcap` and `powercap` libraries; the NVIDIA plugin is built only when the NVML header and library are found. Missing optional dependencies skip that plugin rather than preventing the core from building.

Energy-counter access through MSRs commonly requires root privileges and the `msr` kernel module:

```sh
sudo modprobe msr
sudo ./src/main --aggregate ...
```

The scripts may perform privileged system setup, such as disabling Wi-Fi, and restore changed settings on completion. Without root, the framework can still run workloads and collect accessible telemetry, but energy counters and power capping may be unavailable. A missing metric is reported as `NOT_SUPPORTED`.

Temperature availability depends on the sensors exposed by the kernel and the installed sensor utilities. The CPU and GPU temperatures in the output are direct sensor readings; the CPU temperature is not copied into the GPU field.

### C. Makefile Integration

The `src/Makefile` discovers backend directories below `platform/linux/{cpu,gpu}` and builds implementations for detected hardware as shared objects named `backend.so`, when their dependencies are available. Backend-specific checks, include flags and link dependencies are configured in that Makefile. The main executable is linked with dynamic loading support.

The build separates optional backend dependencies from the core. It checks the CPU vendor and PCI display devices before selecting the built-in backends. New backends configure a build-time detection command in the Makefile, reusing existing checks where applicable. The Intel CPU plugin also requires powercap/raplcap and sensor development files; the NVIDIA plugin requires NVML. If a dependency or device is missing, verbose build output reports the reason and the build continues with the core and other eligible plugins. Some dependencies may need to be linked into the host executable rather than into a plugin, which is the purpose of the `BACKEND_HOST_LDLIBS_*` variables.

Workload directories provide the build and execution conventions used by the experiment scripts. A typical run is initiated with `scripts/measure.sh`, which builds the framework and target workload, then invokes `src/main`. For isolated smoke testing, `scripts/merda.sh` runs the main executable directly in its supported modes and places generated outputs in a temporary directory.

To add a backend, a contributor creates a directory under the appropriate CPU or GPU category, implements the shared API, exports the descriptor, and adds build metadata as needed. The core should not be modified to add a vendor-specific branch or symbol.

### D. Calibration Modes

GreenMeasurer provides two calibration workflows.

**Temperature calibration (`--temperature-calibration`).** The framework waits for an idle period, reads available CPU temperatures and writes the baseline file configured with `calibration_file` (or `-calib-file`). Aggregate and over-time modes use this baseline and the configured variance threshold to avoid running under temperatures that are too far above the reference. All measurement and calibration modes require a readable CPU temperature before starting the workload. This calibration controls one source of experimental variation; it does not eliminate all system noise.

```sh
sudo ./src/main --temperature-calibration \
  -sleep-secs 20 \
  -calib-file /tmp/cores_temperatures.txt
```

**Power-cap calibration (`--powercap-calibration`).** The CPU backend sweeps the requested power range and records the configured limits in a CSV. It invokes the repository's post-processing utility when available. Power capping is optional and currently available only on Intel CPUs. The executable validates this at run time: if the selected CPU backend has no power-cap callbacks, calibration and capped runs abort with an error instead of running uncapped. A successful capped measurement leaves its cap active; a successful calibration disables the RAPL package zones, including previously enabled limits. Failures attempt to restore the prior state. Firmware limits may still apply.

```sh
sudo ./src/main --powercap-calibration \
  -range 15-25 \
  -variance 5 \
  -time_out_limit 1 \
  -secs_to_sleep 10 \
  -n_times 3 \
  -command "./program arg" \
  -language C \
  -program example \
  -output powercap_calibration.csv
```

The timeout value is expressed in hours by the current command-line implementation. Use power-cap values supported by the host processor; the example range is illustrative, not universally valid.

### E. Over-Time Measurements

The `--over-time` mode (over-time sampling) runs the target command while the parent periodically samples the active backends and system metrics without sampler threads. The default pause is 1 ms. Each row reports energy counter snapshots converted to joules and the latest sequentially read gauges; the final row is recorded after the workload exits. `Timestamp_ms` is the monotonic elapsed time in milliseconds at the beginning of each sample's backend reads. The pause does not guarantee that each hardware update is captured.

```sh
sudo ./src/main --over-time \
  -command "./program arg" \
  -interval-ms 100 \
  -language C \
  -program example \
  -variance 5 \
  -sleep-secs 5 \
  -output measurements_ot.csv
```

The schema begins with framework metadata and then includes metrics declared by
the selected backends. Metric columns use `<category>.<name>_<unit>`:

```csv
Language,Program,Powercap,Timestamp_ms,CPU_Memory_KB,Cores,Workload_Processes,System_Running_Processes,Core0_Freq_MHz,...,cpu.Package_J,cpu.Core_J,cpu.DRAM_J,cpu.Temperature_C,gpu.GPU_J,gpu.Power_W,gpu.Temperature_C,gpu.Utilization_%,gpu.VRAM_MiB
```

The example shows one possible selection; exact columns vary by hardware and
backend. Window energy fields are counter readings in joules; a backend without
a counter reader reports `NOT_SUPPORTED`.
Process-scoped peak metrics are polled on aggregate
runs, while process-scoped values in over-time mode are sampled at each tick.
Unsupported counters or sensors are emitted as `NOT_SUPPORTED`. Short sampling
windows also reduce the chance of multiple counter wraps occurring between
observations, though this does not remove all hardware-counter limitations.

### F. Measurement Pipeline

The normal experiment pipeline is:

1. Build the core and available backend plugins with `make -C src`.
2. Build the target workload using its workload Makefile or supplied compile command.
3. Optionally load the MSR module and configure system conditions with the wrapper script.
4. Calibrate CPU temperature or reuse a previously generated baseline.
5. Discover and select the available CPU and GPU plugins.
6. Initialize the selected backends and apply power capping if requested and supported.
7. Execute the workload in aggregate or over-time mode.
8. Record available energy, timing, memory and temperature data in CSV.
9. Release backend resources and restore system settings managed by the wrapper.

The `main.c` implementation is responsible for command parsing, backend discovery, measurement orchestration and output. Plugins own hardware-specific detection and counter access. Sampling failures are checked; they must not be converted into plausible-looking zero values. 32-bit energy-counter deltas use modular unsigned subtraction, and backend validity flags prevent invalid before/after reads from being treated as valid measurements. A single wrap can be handled this way; multiple wraps within one interval cannot be distinguished.

For regular measurements, `measurements.csv` begins with:

```csv
Language,Program,Powercap,Time_ms,CPU_Memory_KB
```

The selected backend metrics follow as `<category>.<name>_<unit>` columns.
`Time_ms` is execution duration in milliseconds, `CPU_Memory_KB` is maximum RSS in KiB
reported by `wait4()` (`ru_maxrss`) for that execution, not a simultaneous process-tree sum,
and each metric follows its
backend-declared sampling semantics. Aggregate-mode peak metrics are polled every
50 ms, so shorter-lived values may be missed. Package energy is a
hardware-domain measurement and must not be interpreted as process-attributed
energy. Package and subdomain values should not be blindly summed because
domains may overlap. The NVIDIA implementation uses per-process NVML data; if
that is unavailable, the CSV contains `NOT_SUPPORTED` rather than whole-device
usage that could include other Slurm jobs.

At startup the executable prints a backend table (by default selected backends and plugin load/descriptor errors; `GREENMEASURER_VERBOSE_BACKENDS=1` lists all) with diagnostic states such as `available`, `not detected`, `not built`, `load failed` and `invalid`. If a category has no selected backend, it warns that the corresponding backend metrics are unavailable. The legacy runner exits if neither category is usable; configured system/external over-time measurements can run without either.

The fake-backend tests can be run with:

```sh
make -C src test
```

They exercise priority selection, detection failure diagnostics, invalid descriptors, absent plugin files, missing categories and the backend-directory override. These tests validate plugin orchestration, not the physical accuracy of hardware energy counters.

## IV. Demonstration

This section defines reproducible demonstrations of the current executable. Smoke tests establish that workflows run and produce output; they are not a substitute for controlled accuracy or performance evaluation. Experiments should report the CPU/GPU model, kernel and driver versions, privilege level, selected-backend table, workload/input, repetitions, power cap, sampling interval and raw CSV files.

### A. Aggregate Measurements

The aggregate mode executes a command repeatedly and writes one result row per run. A minimal smoke test is:

```sh
sudo ./src/main --aggregate \
  -command "sleep 1" \
  -language Shell \
  -program smoke-test \
  -n_times 2 \
  -variance 100 \
  -time_out_limit 1 \
  -sleep_time 1 \
  -powercap -1 \
  -calib-file /tmp/cores_temperatures.txt \
  -output measurements.csv
```

This test checks command execution, repeated rows, CSV shape and availability reporting. It does not characterize application energy, because `sleep` is an idle workload and the counters may include whole-system activity. For a paper-quality experiment, use a deterministic compute workload with documented input, perform warm-up runs where appropriate, repeat enough times to estimate variability, retain raw measurements, and report robust summary statistics.

### B. Over-Time Measurements

A corresponding time-series smoke test is:

```sh
sudo ./src/main --over-time \
  -command "sleep 2" \
  -interval-ms 100 \
  -language Shell \
  -program smoke-test \
  -variance 100 \
  -sleep-secs 1 \
  -calib-file /tmp/cores_temperatures.txt \
  -output measurements_ot.csv
```

The CSV should contain monotonically increasing timestamps and energy counter readings when the selected backend exposes valid counters. Scheduler delays may cause jitter, and hardware counters can wrap or reset. Validate the output by checking the header, row count, timestamps and unsupported fields before plotting it.

The repository's `scripts/merda.sh` is a convenience smoke-test script that invokes the executable directly for temperature calibration, aggregate mode, over-time mode and (when run as root) power-cap calibration. Its simple workload and settings are for functionality checks only. The output should not be presented as a scientific benchmark result. Preserve the generated CSVs and full logs when conducting a formal evaluation.

### C. Evaluation Plan: Comparison with CodeCarbon and EnergiBridge

**Workload.** A recursive Fibonacci implementation in Python (`Languages/Python/Fibonacci`, run with `make aggregate` and `make overtime` for over-time sampling). It is deterministic, CPU-bound and needs no external dependencies. Choose the input (default 35) so that a run lasts long enough (several seconds) for the counters to be meaningful, and keep it identical across tools. The C version (`Languages/C/Fibonacci`) can be added as a second language to show language independence.

**Tools under comparison.** GreenMeasurer, EnergiBridge and CodeCarbon, all on the same machine, with the same workload, input and number of repetitions (e.g. 30). CodeCarbon must wrap the Python call (`EmissionsTracker`), while GreenMeasurer and EnergiBridge execute the command externally. Record the version of every tool.

**Experiments.**

1. *Aggregate mode (total energy per run).* Report mean, standard deviation and coefficient of variation of energy (J) and time (ms) per tool, plus the relative difference between tools. Run in a fixed order or randomised order, with idle periods between runs, and with the same system state (Wi-Fi off, no background load). Test the statistical differences (e.g. Shapiro–Wilk, then Mann–Whitney U or t-test, with effect size).
2. *Over-time mode (energy profile).* Plot counter readings against `Timestamp_ms` for GreenMeasurer and compare their differences with experiment 1, accounting for counter wrap or reset. Interval-average power can be estimated from successive readings and timestamps, with timing uncertainty from sequential backend reads.
3. *Tool overhead.* Compare the energy and time of the workload run with each tool against a run without any tool, and the energy of an idle period of the same length.
4. *Usability and extensibility.* Compare the steps and lines of configuration needed to measure the same workload, and the effort to add a new backend (number of files and lines changed in GreenMeasurer, with no change to `main.c`). Present the result in a feature table (language independence, source change needed, CPU/GPU support, over-time mode, root requirement, behaviour when a counter is missing).
5. *Portability (optional, depending on available machines).* Repeat experiment 1 on each available system (different CPU, with and without NVIDIA GPU) and report which backends were detected and how unsupported ones were reported.

**Threats to validity.** Counters measure hardware domains rather than the process, so background activity is included; the three tools may read different domains (package vs. whole system vs. modelled values), so differences in absolute values must be explained rather than assumed to be errors; results are limited to the machines and Linux versions tested; AMD CPUs are not covered.

## V. Conclusions and Future Work

GreenMeasurer provides a command-oriented workflow for collecting software execution telemetry without modifying the target program. The current implementation combines aggregate repeated measurements, over-time energy sampling, temperature calibration, optional CPU power-cap calibration and dynamically loaded hardware backends. Separating the core from backend implementations is intended to make hardware support extensible while keeping the measurement orchestrator vendor-neutral.

Several limitations must be considered when interpreting measurements. Hardware energy counters often describe system domains rather than the target process; access to MSRs and power controls can require root; sensor exposure varies by machine; and a counter's resolution and wrap behavior constrain sampling. The NVIDIA plugin has not been built or validated in the development environment described in the repository history. The API also lacks an explicit ABI version, so core and plugins should be built from compatible source.

Future work includes:

1. Adding and validating an AMD CPU backend.
2. Comparing measurements against an external power meter and reporting uncertainty.
3. Evaluating repeatability across workloads, hardware models and operating-system versions.
4. Validating optional backends on their target hardware, including the NVML plugin.
5. Adding an API/ABI version to the plugin descriptor and a supported installation target.
6. Extending backend coverage while documenting domain overlap and platform-specific semantics.

Before submission, the authors should update the bibliography, replace the demonstration protocol with retained raw data and statistical results, and ensure every claim in the abstract and conclusion is supported by experiments on the stated hardware and software versions.


## Appendix A. Command-line options

All modes accept `-command` (alias `-run_cmd`) with the command to run through `/bin/sh -c`.

| Option | Unit / type | Modes | Meaning |
|---|---|---|---|
| `-language` | text | aggregate, over-time, power-cap calibration | Label written to the `Language` column; does not change how the command runs (default `C` in aggregate mode) |
| `-program` | text | same | Label written to the `Program` column (default `Target` in aggregate mode) |
| `-n_times` | count | aggregate (default 1), power-cap calibration (default 3) | Number of measured runs (per power limit in calibration) |
| `-variance` | °C | all measurement modes | Maximum temperature above the calibrated baseline at which a run may start; otherwise the tool waits, checking each second (defaults: 0 aggregate, 5 over-time and calibration). A readable CPU temperature is required |
| `-time_out_limit` | hours | aggregate, power-cap calibration | Time limit for the initial check run; on expiry a `TimeOut` row is recorded (default 1) |
| `-interval-ms` | ms | over-time | Pause between sampling cycles (default 1) |
| `-powercap` | W, or `-1` | aggregate, over-time | Package power limit (Intel only); `-1` applies the saved per-machine baseline (default); otherwise at least 1 |
| `-sleep_time`, `-sleep-secs` | s | all | Idle time used to settle the machine when a temperature baseline has to be created (default 5; default 10 as `-secs_to_sleep` in power-cap calibration) |
| `-calib-file` | path | all | Baseline-temperature file; the path must be supplied in the TOML or as a command-line option, and is created by calibration if missing |
| `-output`, `-measurements-file` | path | all | Output CSV (defaults `measurements.csv`, `measurements_ot.csv`, `powercap_calibration.csv`) |
| `-range` | `min-max`, W | power-cap calibration | Range of limits to test; minimum at least 1 W |
| `-compile_cmd` | text | power-cap calibration | Optional command run once before the sweep |

For the current interface, `./src/main --list-metrics [cpu|gpu [directory]]`
lists metrics, functions, descriptions and origins from compiled backend
plugins without hardware detection. Built-in backend metrics are declared in a
single registry; see the [README](../README.md#adding-a-backend) for the
templates and extension guide. `GREENMEASURER_BACKEND_DIR` overrides the plugin
directory, and `GREENMEASURER_VERBOSE_BACKENDS=1` shows backend states at
measurement startup. Load only trusted plugins.

## Appendix B. Output columns

| Column | Unit | Type | Source | Meaning |
|---|---|---|---|---|
| `Language`, `Program` | text | core | options | Labels |
| `Powercap` | W | core | option | Cap used, or `-1` |
| `Time_ms` | ms | core, aggregate | wall clock | Duration of the run |
| `Timestamp_ms` | ms | core, over-time | monotonic clock | Elapsed time at the start of each sample's backend reads |
| `CPU_Memory_KB` | KiB | core, aggregate | `wait4()` / `ru_maxrss` | Maximum RSS reported for the measured run, not a simultaneous process-tree sum |
| `cpu.Package_J` | J | window | Intel | Energy of the CPU package |
| `cpu.Core_J` | J | window | Intel | Energy of the cores (part of the package) |
| `cpu.DRAM_J` | J | window | Intel | Energy of the memory |
| `cpu.Temperature_C` | °C | gauge | Intel | Average core temperature |
| `gpu.GPU_J` | J | window | NVIDIA, Intel | GPU energy |
| `gpu.Power_W` | W | gauge | NVIDIA | Instantaneous GPU power |
| `gpu.Temperature_C` | °C | gauge | NVIDIA, Intel | GPU temperature |
| `gpu.Utilization_%` | % | gauge | NVIDIA | GPU utilization |
| `gpu.VRAM_MiB` | MiB | peak | NVIDIA | GPU memory used by the process (highest value seen) |

A *window* metric is the difference of a counter between the start and the end of the run (or since the preceding reading in over-time mode); a *gauge* is a single reading (at the end of the run, or at each sample in over-time mode); a *peak* is the highest reading seen while the command ran. An unavailable value is written as `NOT_SUPPORTED`, never as zero.
