# Aspect configuration examples

Each `config.toml` declares available aspects, not a mode or workload. A metric
aspect declares one metric; a powercap control aspect declares hooks without a
metric. Select the mode, command, options and aspect flags when invoking the
executable.

- `intel/config.toml`: Intel CPU and GPU readers and Intel powercap control.
- `nvidia/config.toml`: NVIDIA GPU readers.
- `system/config.toml`: system readers and wall-clock time.
- `config.toml`: all device and system readers together and Intel powercap control.

The NVIDIA and system examples do not depend on Intel libraries. To combine
their aspects with Intel powercap or temperature control, use the combined
configuration or declare those aspects explicitly in your own TOML.

For example:

```sh
make -C src
python3 scripts/weave.py examples/system/config.toml -o generated-example-system
./generated-example-system/greenmeasurer --aggregate \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -language Python -program fibonacci-30 -output example-system.csv \
  -n_times 1 -sleep-secs 0 -time_out_limit 0 --Time.WallClock --System.Cores
```

Generate again after changing a TOML. Header and library paths are relative to
that TOML; workload, output and calibration paths are relative to the runtime
working directory. Use `--help` to list the generated aspect flags.
`Time.WallClock` is an explicitly selected `CLOCK_MONOTONIC` duration aspect;
other examples do not implicitly measure execution time.

Temperature aspects can be measured without thermal control. To control a
measurement, pass `-temperature-control ASPECT` and `-calib-file ASPECT=PATH` for
each controlled sensor, plus `-variance`. The explicit calibration mode
overwrites the named file, and a missing baseline is calibrated automatically
during measurement. Choose a suitable stabilization time for real experiments.

The weaver skips aspects whose headers or libraries are missing or whose code
cannot be built, with diagnostics. Use `--help` to list the resulting flags.
A failed setup emits a warning and removes the aspect from the run, including
its CSV column; unavailable readings from initialized aspects are
`NOT_SUPPORTED`. An unavailable controlled temperature disables its thermal
control and repetition pauses, with a warning; other temperatures are still
controlled. Explicit calibration requires available sensors. Intel and NVIDIA
readings require suitable hardware, dependencies and permissions.

**Powercap changes CPU limits.** Choose limits appropriate for the machine;
example values are not recommendations. Select the declared control aspect
explicitly (for example `--IntelCPU.Powercap`) for powercap calibration. Intel
energy readers, temperature reader and powercap control use separate libraries.
Omitting temperature or powercap removes the respective dependencies. For an aggregate-mode smoke test of all
TOMLs, see [test_examples.sh](../scripts/README.md#check-all-examples). See the
[weaver guide](../docs/WEAVER.md) for all modes, fields and options.
