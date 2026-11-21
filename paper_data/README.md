# Archived experiment data

The CSVs, reports, notebooks and TOML files in this directory document earlier
experiments. The TOML files use the previous configuration format and are kept
as historical records; they cannot be used with the current generator. The
archived aggregate configuration has been renamed for current terminology,
not migrated to the current format.
To rerun these experiments with the current format, see
[configurations-paper](../configurations-paper). For current examples, see
[examples](../examples).

The archived dataset used recursive Fibonacci in C with input 55, 10 repetitions,
a four-hour timeout, 5 C temperature variance, a 60-second idle period and
power caps of -1 and 4 W.

- `cores_temperatures.txt`: temperature calibration
- `powercap_report.txt`: powercap average package report
- `csv_files/`: measurements, calibration and performance statistics
- `ipynb_files/`: analysis notebooks and their plots
