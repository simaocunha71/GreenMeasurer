# Fibonacci experiment

`config.toml` declares the aspects used by `run.sh`. From the repository root:

```sh
bash configurations-generated/run.sh [new-build-directory]
```

The script builds the device libraries and one executable, then runs CPU
temperature calibration, powercap calibration, aggregate and over-time, in that
order. It measures Python Fibonacci 42 ten times per limit, with 60-second waits.
Powercap calibration tests the saved baseline and 5–15 W; aggregate and over-time
test the saved baseline (`-1`) and 5 W. The saved baseline is not necessarily
uncapped. Thermal control uses only the CPU baseline, with variance 5 C for
powercap calibration and aggregate and 15 C for over-time. GPU temperature is
recorded as a metric but is not used for thermal control. Powercap calibration
has a one-hour timeout; aggregate and over-time have no timeout.

The sequence requires the declared Intel CPU, Intel GPU and NVIDIA libraries,
including NVML, and a readable CPU temperature sensor for calibration. Missing aspects
are skipped at build time, but the script stops if a required flag is unavailable.
It does not obtain elevated privileges automatically.

**Powercap changes CPU limits.** Review the settings, hardware and permissions
before running. The script overwrites `cores_temperatures.txt` in the repository
root. Aggregate and over-time append
to their CSVs; powercap calibration may replace its CSV. The default build
directory, `generated-fibonacci-sequence`, must not already exist.
