### Archived Dataset Parameters

These notes describe the archived experiment dataset and its original
parameters. They do not describe the current example configurations in this
directory. The parameter names and
command-line workflow are historical; for the current TOML workflow, see the
[project README](../README.md).

#### Parameters

- **Program**: Fibonacci (recursive) in C with input `55`
- **n_times**: `10` — Number of times to execute each program
- **time_out_limit**: `4` — Time limit (in hours) for each program execution
- **variance**: `5` — CPU temperature variance
- **sleep_secs**: `60` — Seconds for idle execution and temperature calibration
- **interval**: `100` — Interval for over-time measurements (in milliseconds)
- **power_caps**: `(-1, 4)` — Power cap values

#### Data Files Index

- **Temperature Calibration**
  - `cores_temperatures.txt` — Temperature calibration text file
- **Powercap Report**
  - `powercap_report.txt` — Powercap Average Package Report
- **CSV Files**
  - `csv_files/powercap_calibration.csv` — Powercap calibration CSV
  - `csv_files/measurements.csv` — Standard measurements CSV
  - `csv_files/measurements_ot.csv` — Over-time measurements CSV
  - `csv_files/perf_stats.csv` — Performance measurements CSV
- **Jupyter Notebooks**
  - `ipynb_files/notebook_ot.ipynb` — Graphs for over-time measurements
  - `ipynb_files/notebook_perf.ipynb` — Graphs for performance measurements
  - `ipynb_files/notebook_std.ipynb` — Graphs for standard measurements

### Current Paper Configuration Set

The four TOML files in this directory configure temperature calibration,
powercap calibration from 1 to 25 W, standard measurements, and over-time
measurements. The standard and over-time configurations select all metrics
and specify `powercap = [-1, 5]`, covering both settings in one invocation.
The workload is the repository's Python Fibonacci program with input 42.

Standard and powercap calibration use `repetitions = 10` (the current TOML
equivalent of the historical `n_times`); standard mode applies it to each
power-cap setting. Over-time mode runs once per power-cap setting per
invocation; `repetitions` is not valid for that mode, so repeat the invocation
to collect independent runs. Each mode writes both settings to one CSV,
distinguished by the `Powercap` column. The over-time configuration requests
`interval_us = 10000`, a pause of 10 milliseconds; actual sample
periods are longer because metric reads and scheduling take time. Measurement
configurations use `variance = 5` and `sleep_seconds = 60`. The temperature
baseline is shared by the configurations and should be generated before
measurements. The `-1` settings mean GreenMeasurer does not apply a cap; they
do not guarantee that the host has no pre-existing power limits. The 10 ms
over-time interval is the requested pause, not a guaranteed sampling period.

Run temperature calibration first; it executes directly rather than generating
an executable:

```sh
./src/main --config paper_data/temperature-calibration.toml
bash scripts/measure.sh generate generated-paper \
  paper_data/powercap-calibration.toml \
  paper_data/standard.toml paper_data/overtime.toml
bash scripts/measure.sh run generated-paper
```

Run these commands from the repository root. Generation does not run the three
measurements, and `generated-paper` must not already exist. Temperature
calibration and measurements require compatible hardware; power-cap operations
also require support and appropriate permissions. The configured output files
are overwritten on successful runs. The archived CSV files listed above are
from a different experiment and are not results of these configurations.
