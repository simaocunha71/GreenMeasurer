# Scripts

Python 3.11 or Python with `tomli` is required. The weaver and generated
executable are documented in [WEAVER.md](../docs/WEAVER.md).

## Batch generation and execution

```sh
bash scripts/measure.sh generate generated-batch first.toml second.toml
bash scripts/measure.sh run generated-batch -- \
  -command "true" -program true -output results.csv \
  -n_times 1 -sleep-secs 0 -timeout-hours 0 --System.Cores
```

Each `generate` argument builds one executable: a TOML file is woven alone and
a directory weaves all its `*.toml` aspect files together. The batch directory
must not already exist. Generation does not run workloads.
`run` passes the same explicit arguments to each executable in order, so every
selected flag must exist in each configuration and the woven modes must accept
the same options. Use compatible CSV schemas or run experiments separately. An
incomplete batch cannot be run.

`run_powercap_calibration.sh new-directory aspect.toml [aspect.toml ...]`
generates an executable and checks that a declared powercap calibration aspect
was built. Run it separately with `--powercap-calibration`, its explicit
options and the control aspect flag, such as `--IntelCPU.Powercap`.

## Check all examples

```sh
bash scripts/test_examples.sh
```

The script weaves each example configuration (a directory of aspect TOMLs is
one configuration) and performs a `true` workload in the mode declared by the
TOMLs, explicitly selecting every built metric and the first built powercap
control aspect; additional and calibration-only controls are skipped. It uses separate result directories
and temperature baseline files, creating missing baselines with no wait. It
does not perform powercap sweeps; `-1` can still apply the saved hardware
baseline if a powercap control is selected. Review the TOMLs, hardware and
permissions before running. An unavailable controlled temperature disables its
thermal control. `WARN` means a warning or
`ERROR` appeared; `FAIL` means
generation or execution failed. `-o DIR` selects the results directory and
`-s` stops on the first failure.

## Fibonacci sequence

```sh
bash configurations-paper/run.sh [new-build-directory]
```

The script in `configurations-paper/` builds the device libraries and weaves
that directory's aspect files into one aggregate and one over-time executable.
It calibrates CPU temperature, then runs powercap calibration, aggregate and
over-time with explicit arguments; see
[configurations-paper/README.md](../configurations-paper/README.md) for the
exact settings. The script can take a long time and **changes CPU limits**.
Review its settings and use suitable hardware and permissions.

The sequence selects every incorporated metric and requires the Intel powercap
control, the Intel CPU temperature and the NVIDIA GPU temperature, so the
necessary libraries, including NVML, must be present. NVIDIA temperature is
measured but not used for thermal control. Aggregate and over-time append to
their CSVs; calibration may replace its output. Save prior results first. The
default build directory is `generated-paper-experiments` and must not already
exist.

## Overhead and timing analysis

```sh
python3 scripts/compare_aggregate_overhead.py \
  --config examples/aggregate/system.toml examples/aggregate/time.toml \
  --command "sleep 0.1" --time-aspect Time.WallClock \
  --metrics System.Cores System.RunningProcesses --runs 10
python3 scripts/compare_overtime_overhead.py \
  --config examples/over-time/system.toml \
  --command "sleep 0.1" --metrics System.Cores --interval-ms 5 --runs 5
python3 scripts/analyze_overtime.py measurements.csv --threshold-us 1000
```

The comparisons select the listed metrics and measure wall time, not energy
overhead. `--config` takes the aspect TOMLs to weave; their declared mode must
match the script (aggregate or over-time). The aggregate comparison also
requires a declared millisecond duration metric via `--time-aspect`; it is
selected for each measured run, separately from the metrics progressively
added. For thermal control, pass `--temperature-control METRIC` and
`--calib-file METRIC=PATH` for each controlled sensor, plus `--variance`; the
files must already exist. Temperature metrics without control need no calibration. Each comparison
uses one repetition and temporary CSVs, without selecting a powercap control aspect.
Existing hardware limits and thermal waiting can affect timing.

`probe_rapl_frequency.py` observes Intel RAPL counter changes and requires
Linux MSR access. `install.sh` installs build and optional device dependencies.
