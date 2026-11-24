# Available aspects and usage

This directory lists and demonstrates the aspects supported in this version of
GreenMeasurer. Each TOML file declares one aspect in the paper's format: a
`[measurement]` table (aspect name, mode, setup/teardown or control hooks), a
`[device]` table (library and header), and `[[metric]]` entries.

Because measurement mode is part of the aspect definition, the same aspects are
provided in two directories:

- `aggregate/`: aspects woven into an aggregate-mode executable.
- `over-time/`: the same aspects woven into an over-time executable.

## Aspects in this version

Each mode directory contains:

- `intel-cpu.toml`: Intel CPU energy metrics (`IntelCPU.Package`, `IntelCPU.Core`,
  `IntelCPU.DRAM`).
- `intel-cpu-temperature.toml`: Intel CPU temperature (thermal metric).
- `intel-cpu-powercap.toml`: Intel powercap control aspect (no metrics, no mode).
- `intel-gpu.toml`: Intel GPU energy and temperature.
- `nvidia-gpu.toml`: NVIDIA GPU energy, power, temperature, utilization, and
  VRAM.
- `system.toml`: system metrics (cores, frequency, processes, workload memory).
- `time.toml`: wall-clock execution time (`Time.WallClock` duration metric).

## Building and weaving aspects

Build the device support code and weave only the TOML files you need into an
executable for the chosen mode:

```sh
make -C src
python3 scripts/weave.py \
  available-aspects/aggregate/system.toml \
  available-aspects/aggregate/time.toml \
  -o generated-aggregate-system-time

./generated-aggregate-system-time/greenmeasurer --help
```

Run `./generated-.../greenmeasurer --help` after weaving to inspect the exact
metric and control flags incorporated for your hardware, libraries, and
permissions. Re-run `scripts/weave.py` whenever you edit a TOML file or change
the selected aspects.

## Usage examples across aspects

`Time.WallClock` is an explicitly selected `CLOCK_MONOTONIC` duration metric;
other aspects do not implicitly measure execution time. Pass only the `--Metric`
flags exposed by your generated binary.

### System and wall-clock time (aggregate)

```sh
./generated-aggregate-system-time/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-system-time.csv \
  -n_times 1 -sleep-secs 30 -timeout-hours 4 \
  --Time.WallClock --System.Cores
```

### Intel CPU energy and temperature

```sh
python3 scripts/weave.py \
  available-aspects/aggregate/time.toml \
  available-aspects/aggregate/intel-cpu.toml \
  available-aspects/aggregate/intel-cpu-temperature.toml \
  -o generated-aggregate-intel-cpu

./generated-aggregate-intel-cpu/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-intel-cpu.csv \
  -n_times 1 -sleep-secs 30 -timeout-hours 4 \
  --Time.WallClock --IntelCPU.Package --IntelCPU.Core --IntelCPU.DRAM \
  --IntelCPU.Temperature
```

To enable thermal control during measurement (rather than measuring temperature
alone), pass `-temperature-control METRIC`, `-calib-file METRIC=PATH` for each
controlled sensor, and `-variance`:

```sh
./generated-aggregate-intel-cpu/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-intel-cpu-thermal.csv \
  -n_times 1 -sleep-secs 30 -timeout-hours 4 \
  --Time.WallClock --IntelCPU.Package --IntelCPU.Temperature \
  -temperature-control IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=intel-cpu-temp.calib \
  -variance 15
```

### Intel CPU powercap control

`intel-cpu-powercap.toml` declares a control aspect with no metrics and no mode,
so it can be combined with either `aggregate` or `over-time`:

```sh
python3 scripts/weave.py \
  available-aspects/aggregate/time.toml \
  available-aspects/aggregate/intel-cpu.toml \
  available-aspects/aggregate/intel-cpu-powercap.toml \
  -o generated-aggregate-powercap

./generated-aggregate-powercap/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-powercap.csv \
  -n_times 1 -sleep-secs 30 -timeout-hours 4 \
  --Time.WallClock --IntelCPU.Package --IntelCPU.Powercap \
  -powercap -1,15
```

### Intel GPU and NVIDIA GPU

```sh
python3 scripts/weave.py \
  available-aspects/aggregate/time.toml \
  available-aspects/aggregate/intel-gpu.toml \
  available-aspects/aggregate/nvidia-gpu.toml \
  -o generated-aggregate-gpus

./generated-aggregate-gpus/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-gpus.csv \
  -n_times 1 -sleep-secs 30 -timeout-hours 4 \
  --Time.WallClock
```

Check `./generated-aggregate-gpus/greenmeasurer --help` to see which Intel GPU
and NVIDIA GPU metric flags were built on your machine before passing them on
the command line.

### Over-time sampling mode

```sh
python3 scripts/weave.py \
  available-aspects/over-time/time.toml \
  available-aspects/over-time/system.toml \
  -o generated-over-time-system

./generated-over-time-system/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output example-over-time.csv \
  -n_times 1 -sleep-secs 30 -timeout-hours 4 \
  -interval-ms 10 --Time.WallClock --System.Cores
```

## Rules, failures, and safety notes

- **Mode compatibility:** All metric aspects woven into a single binary must
  declare the same mode (`aggregate` or `over-time`). The powercap control
  aspect declares no mode and can be combined with either.
- **Path resolution:** Header and library paths in a TOML file are relative to
  that TOML file; workload, output, and calibration paths are relative to the
  runtime working directory.
- **Calibration behavior:** Explicit calibration overwrites the target file; a
  missing calibration file during measurement is calibrated automatically. Choose
  a suitable stabilization time for real experiments.
- **Missing dependencies and runtime errors:** The weaver skips aspects with
  missing headers or libraries or build failures, emitting diagnostics; check
  `--help` for resulting flags. A setup failure at runtime emits a warning and
  removes the aspect (including its CSV columns); unavailable readings from an
  initialized aspect appear as `ERROR`. If a controlled temperature sensor is
  unavailable, its thermal control is disabled with a warning while other
  controlled temperatures remain active. Explicit calibration requires available
  sensors, and Intel/NVIDIA readings require suitable hardware, dependencies, and
  permissions.
- **Powercap warning:** **Powercap changes real CPU limits.** Choose limits
  appropriate for the machine; example values are not recommendations. Select
  the declared control aspect explicitly (for example `--IntelCPU.Powercap`) for
  powercap calibration. Intel energy readers, temperature reader, and powercap
  control use separate libraries; omitting temperature or powercap removes the
  respective dependencies.

## Related references

- For a guided end-to-end walkthrough across progressive aspect selections, see
  `quickstart/README.md`.
- For smoke testing across examples, see
  [test_examples.sh](../scripts/README.md#check-all-examples).
- For all modes, TOML fields, and CLI options, see the
  [weaver guide](../docs/WEAVER.md).