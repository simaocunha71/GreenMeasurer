# Aspect configuration examples

Each TOML file declares one aspect in the paper's format: a `[measurement]`
table (aspect name, mode, setup/teardown or control hooks), a `[device]` table
(library and header) and `[[metric]]` entries. The measurement mode is part of
the aspect, so the same aspects exist in two variants:

- `aggregate/`: aspects woven into an aggregate-mode executable.
- `over-time/`: the same aspects woven into an over-time executable.

Each directory contains:

- `intel-cpu.toml`: Intel CPU energy metrics (`IntelCPU.Package`, `IntelCPU.Core`,
  `IntelCPU.DRAM`).
- `intel-cpu-temperature.toml`: Intel CPU temperature (thermal metric).
- `intel-cpu-powercap.toml`: Intel powercap control aspect (no metrics, no mode).
- `intel-gpu.toml`: Intel GPU energy and temperature.
- `nvidia-gpu.toml`: NVIDIA GPU energy, power, temperature, utilization and VRAM.
- `system.toml`: system metrics (cores, frequency, processes, workload memory).
- `time.toml`: wall-clock execution time (`Time.WallClock` duration metric).

Weave the aspects you need; pass several files to combine them. For example:

```sh
make -C src
python3 scripts/weave.py examples/aggregate/system.toml examples/aggregate/time.toml \
  -o generated-example-system
./generated-example-system/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-system.csv \
  -n_times 1 -sleep-secs 0 -timeout-hours 0 --Time.WallClock --System.Cores
```

Generate again after changing a TOML. Header and library paths are relative to
that TOML; workload, output and calibration paths are relative to the runtime
working directory. Use `--help` to list the generated metric flags.
`Time.WallClock` is an explicitly selected `CLOCK_MONOTONIC` duration metric;
other examples do not implicitly measure execution time. All metric aspects
woven together must declare the same mode; the powercap control aspect declares
no mode and can be combined with either.

Temperature metrics can be measured without thermal control. To control a
measurement, pass `-temperature-control METRIC` and `-calib-file METRIC=PATH` for
each controlled sensor, plus `-variance`. The explicit calibration mode
overwrites the named file, and a missing baseline is calibrated automatically
during measurement. Choose a suitable stabilization time for real experiments.

The weaver skips aspects whose headers or libraries are missing or whose code
cannot be built, with diagnostics. Use `--help` to list the resulting flags.
A failed setup emits a warning and removes the aspect from the run, including
its CSV columns; unavailable readings from initialized aspects are
`ERROR`. An unavailable controlled temperature disables its thermal
control, with a warning; other temperatures are still
controlled. Explicit calibration requires available sensors. Intel and NVIDIA
readings require suitable hardware, dependencies and permissions.

**Powercap changes CPU limits.** Choose limits appropriate for the machine;
example values are not recommendations. Select the declared control aspect
explicitly (for example `--IntelCPU.Powercap`) for powercap calibration. Intel
energy readers, temperature reader and powercap control use separate libraries.
Omitting temperature or powercap removes the respective dependencies. For a
smoke test of all examples, see
[test_examples.sh](../scripts/README.md#check-all-examples). See the
[weaver guide](../docs/WEAVER.md) for all modes, fields and options.
