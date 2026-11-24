# Paper experiments

`run.sh` reproduces the experiments of the paper's usage example (temperature
calibration, powercap calibration, aggregate, and over-time) using the current
aspect format and CLI. The aspect TOMLs used by the script live in this
directory, in one variant per mode:

- `aggregate/` and `over-time/`:
  - `intel-cpu.toml`: Intel CPU package energy (`IntelCPU.Package`).
  - `intel-cpu-temperature.toml`: Intel CPU temperature (used for temperature
    control).
  - `intel-cpu-powercap.toml`: Intel powercap control aspect (no mode; required
    by the powercap stages).
  - `system.toml`: number of running processes in the system
    (`System.RunningProcesses`).
  - `time.toml`: wall-clock execution time (`Time.WallClock`; also required by
    the powercap speedup/greenup report).

From the repository root:

```sh
bash paper-experiments/run.sh [new-build-directory]
```

The script builds the device libraries and weaves `paper-experiments/aggregate/`
and `paper-experiments/over-time/` into two subdirectories inside the build
directory (default `generated-paper-experiments/`):

- `run-0001/`: contains the aggregate-mode executable (`greenmeasurer`) and its
  generated source (`GreenMeasurer.c`).
- `run-0002/`: contains the over-time-mode executable (`greenmeasurer`) and its
  generated C sources (`GreenMeasurer.c`, `IntelCPU.c`,
  `IntelCPU_Powercap.c`, `IntelCPU_Temperature.c`, `System.c`, and `Time.c`).

It then runs, in order:

1. **Temperature calibration**: 30-second sleep time (`-sleep-secs 30`);
   writes the CPU baseline to `paper-experiments/temperature-baseline.txt`.
2. **Powercap calibration**: sweeps limits in `5-15 W` (`-range 5-15`) alongside
   no cap (`-1`), 10 repetitions per level (`-n_times 10`), 30-second sleep
   between runs, 4-hour timeout. The speedup/greenup report records the
   optimal powercap (lowest package energy) in
   `paper-experiments/powercap-calibration-optimal-powercap.txt`.
3. **Aggregate**: runs no cap (`-1`) and the calibrated optimal powercap (or
   only `-1` when optimal is `-1`), 10 repetitions per level (`-n_times 10`),
   30-second sleep between runs, 4-hour timeout.
4. **Over-time**: runs no cap (`-1`) and the calibrated optimal powercap (or
   only `-1` when optimal is `-1`), 1 run per level (`-n_times 1`), 10 ms
   sampling interval (`-interval-ms 10`), 30-second sleep between runs, 4-hour
   timeout.

Every measurement runs Python recursive Fibonacci 40
(`python3 paper-experiments/fibonacci.py 40`, program name
`fibonacci-40`), selects **all incorporated metrics** (read from `--help`,
excluding `--IntelCPU.Powercap`), uses CPU thermal control with a 10 °C
variance (`-variance 10`) and the baseline from `temperature-baseline.txt`,
and writes its CSV into `paper-experiments/`.

## Generated CSVs

The script writes three CSV files in `paper-experiments/`:

- `powercap-calibration.csv`: one row per repetition across the calibrated
  powercap levels (`-1` and `5-15 W`), with columns `Program`, `Powercap`,
  `IntelCPU.Temperature_C`, `IntelCPU.Package_J`,
  `System.RunningProcesses_processes`, and `Time.WallClock_ms`.
- `aggregate.csv`: one row per repetition across the selected powercap levels
  (no cap `-1` and the optimal powercap recorded in
  `powercap-calibration-optimal-powercap.txt`), with the same columns as
  `powercap-calibration.csv`.
- `overtime.csv`: time-series samples collected at `10 ms` intervals for each
  selected powercap level, adding `Timestamp_ms` alongside `Program`,
  `Powercap`, `IntelCPU.Temperature_C`, `IntelCPU.Package_J`,
  `System.RunningProcesses_processes`, and `Time.WallClock_ms`.

## Plots and notebooks

Analysis notebooks and generated PDF figures live in `plots/`:

- `plots/aggregate.ipynb` and `plots/aggregate.pdf`: summary comparison of
  aggregate metrics (`IntelCPU.Package_J`, `Time.WallClock_ms`,
  `IntelCPU.Temperature_C`, and `System.RunningProcesses_processes`) across
  powercap levels.
- `plots/over-time.ipynb` and `plots/over-time.pdf`: time-series plots of
  `IntelCPU.Temperature_C`, `IntelCPU.Package_J`, and
  `System.RunningProcesses_processes` over `Timestamp_ms`.
- `plots/powercap_analysis.ipynb` and `plots/powercap-calibration.pdf`:
  analysis and plot of the powercap calibration sweep.

The script requires `--IntelCPU.Powercap`, `--IntelCPU.Temperature`, and at
least one metric flag to have been incorporated, and stops otherwise.
**Powercap changes real CPU limits** and usually requires appropriate
permissions. The default build directory, `generated-paper-experiments`,
must not already exist.