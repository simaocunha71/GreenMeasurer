# Scripts

Python 3.11 or Python with `tomli` is required. The weaver and generated
executable are documented in [WEAVER.md](../docs/WEAVER.md).

## Batch generation and execution

```sh
bash scripts/measure.sh generate generated-batch first.toml second.toml
bash scripts/measure.sh run generated-batch -- --aggregate \
  -command "true" -language Shell -program true -output results.csv \
  -n_times 1 -sleep-secs 0 -time_out_limit 0 --System.Cores
```

The batch directory must not already exist. Generation does not run workloads.
`run` passes the same explicit arguments to each executable in order, so every
selected flag must exist in each configuration. Use compatible CSV schemas or
run experiments separately. An incomplete batch cannot be run.

`run_powercap_calibration.sh config.toml new-directory` generates an executable
and checks that a declared powercap calibration aspect was built. Run it
separately with `--powercap-calibration`, its explicit options and the control
aspect flag, such as `--IntelCPU.Powercap`.

## Check all examples

```sh
bash scripts/test_examples.sh
```

The script generates each example and performs an aggregate-mode `true` workload,
explicitly selecting every built metric and the first built powercap aspect supporting
aggregate mode; additional and calibration-only controls are skipped. It uses separate result directories
and temperature baseline files, creating missing baselines with no wait. It
does not perform powercap sweeps; `-1` can still apply the saved hardware
baseline if a powercap control is selected. Review the TOMLs, hardware and
permissions before running. An unavailable controlled temperature disables its
thermal control and repetition pauses. `WARN` means a warning or
`NOT_SUPPORTED` appeared; `FAIL` means
generation or execution failed. `-o DIR` selects the results directory and
`-s` stops on the first failure.

## Fibonacci sequence

```sh
bash configurations-generated/run.sh [new-build-directory]
```

The script in `configurations-generated/` builds the device libraries and generates one executable from
`configurations-generated/config.toml` for all modes. It
calibrates CPU temperature, then runs powercap calibration, aggregate and
over-time with explicit arguments. It overwrites `cores_temperatures.txt`, waits
60 seconds for temperature calibration, measures Fibonacci 42 ten times per
limit, and tests `-1,5` W and the 5–15 W calibration range. `-1` applies the saved
baseline, which is not necessarily uncapped. The script can take a long time and **changes
CPU limits**. Review its settings and use suitable hardware and permissions.

The sequence selects all its configured aspects, so the necessary libraries,
including NVML, must be present. Skipped aspects have no flag and the sequence
stops if it tries to select one. The CPU temperature must be readable for
calibration; NVIDIA temperature is measured but not used for thermal control.
Aggregate and over-time append to their CSVs; calibration may replace its
output. Save prior results first. The default build directory is
`generated-fibonacci-sequence` and must not already exist.

## Overhead and timing analysis

```sh
python3 scripts/compare_aggregate_overhead.py --config examples/system/config.toml \
  --command "sleep 0.1" --time-aspect Time.WallClock \
  --metrics System.Cores System.RunningProcesses --runs 10
python3 scripts/compare_overtime_overhead.py --config examples/system/config.toml \
  --command "sleep 0.1" --metrics System.Cores --interval-ms 5 --runs 5
python3 scripts/analyze_overtime.py measurements.csv --threshold-us 1000
```

The comparisons select the listed aspects and measure wall time, not energy
overhead. The aggregate comparison also requires a declared millisecond
duration aspect via `--time-aspect`; it is selected for each measured run,
separately from the metrics progressively added. For thermal control, pass `--temperature-control ASPECT` and `--calib-file
ASPECT=PATH` for each controlled sensor, plus `--variance`; the files must already
exist. Temperature metrics without control need no calibration. Each comparison
uses one repetition and temporary CSVs, without selecting a powercap control aspect.
Existing hardware limits and thermal waiting can affect timing.

`probe_rapl_frequency.py` observes Intel RAPL counter changes and requires
Linux MSR access. `install.sh` installs build and optional device dependencies.
