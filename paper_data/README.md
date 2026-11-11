### Data Used in the Paper

These notes describe the archived experiment dataset and its original
parameters. The parameter names and command-line workflow are historical; for
the current TOML configuration and execution workflow, see the
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
