# Paper experiments

`run.sh` reproduces the experiments of the paper's usage example, previously
described by the archived TOML files in [paper_data](../paper_data)
(`temperature-calibration.toml`, `powercap-calibration.toml`, `aggregate.toml`
and `overtime.toml`). The aspect TOMLs used by the script live in this
directory, in one variant per mode:

- `aggregate/` and `over-time/`:
  - `intel-cpu.toml`: Intel CPU package energy (`IntelCPU.Package`).
  - `intel-cpu-temperature.toml`: Intel CPU temperature (used for thermal
    control).
  - `intel-cpu-powercap.toml`: Intel powercap control aspect (no mode; required
    by the powercap stages).
  - `system.toml`: number of running processes in the system
    (`System.RunningProcesses`).
  - `time.toml`: wall-clock execution time (`Time.WallClock`; also required by
    the powercap speedup/greenup report).

From the repository root:

```sh
bash configurations-paper/run.sh [new-build-directory]
```

The script builds the device libraries, weaves `configurations-paper/aggregate/`
and `configurations-paper/over-time/` into one executable per mode, and then
runs, in order:

1. **Temperature calibration**: 60-second settling time; writes the CPU baseline
   to `configurations-paper/temperature-baseline.txt`.
2. **Powercap calibration**: no cap (`-1`) plus every integer limit from 1 to
   25 W, 10 repetitions per level, 4-hour timeout. The speedup/greenup report
   records the optimal powercap (lowest package energy) in
   `configurations-paper/powercap-calibration-optimal-powercap.txt`.
3. **Aggregate**: no cap and the calibrated optimal powercap, 10 repetitions
   per level, 4-hour timeout.
4. **Over-time**: no cap and the calibrated optimal powercap, one run per
   level, 10 ms sampling interval, 4-hour timeout.

Every measurement runs Python recursive Fibonacci 42, selects **all metrics
incorporated into the executable** (read from `--help`, matching the declared
set above), uses CPU thermal control with a 5 °C variance (which paces
consecutive runs) and a 60-second calibration settling time, and writes its CSV
into `configurations-paper/`.

The script requires the Intel powercap control and the Intel CPU temperature to
have been incorporated, and stops otherwise. **Powercap changes
CPU limits** and usually requires elevated privileges; the sweep over 25 levels
with 10 repetitions can take many hours. Aggregate and over-time append to
their CSVs; powercap calibration may replace its CSV. The default build
directory, `generated-paper-experiments`, must not already exist.

The archived results of the original experiments remain in
[paper_data](../paper_data).
