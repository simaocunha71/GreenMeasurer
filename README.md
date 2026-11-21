# GreenMeasurer

GreenMeasurer generates a Linux measurement executable from TOML aspect files.
Each file declares one aspect under `[measurement]`, `[device]` and `[[metric]]`
tables: the energy-monitoring library to use, the measurement mode, and the
metrics it provides; no aspects are selected automatically. The weaver attempts
to link each declared aspect with a runtime that executes the workload and
writes CSV results; aspects that cannot be built are skipped with a warning.
Without active aspects, it runs the command once with `system()`.

| Authors | Contacts |
|---------|----------|
| Simão Cunha | simao.s.cunha@inesctec.pt |
| João Saraiva | saraiva@di.uminho.pt |

## Requirements

- Python 3.11 or Python 3 with `tomli`
- A C compiler, `make`, and POSIX threads
- Headers, libraries, hardware access, and permissions required by the
  aspects you want to use

Optional example device libraries live in [src/devices](src/devices). Build
the ones whose dependencies are installed with:

```sh
make -C src
```

The Intel CPU temperature aspect needs the libsensors development files; its
energy readers do not. The separate Intel powercap aspect needs powercap and
raplcap development files. Configurations without those aspects do not need
their dependencies. The NVIDIA example needs NVML headers and its runtime
library. Missing optional headers cause the affected library to be skipped.

## Generate and run

For example, after building the system device library:

```sh
python3 scripts/weave.py examples/aggregate/system.toml examples/aggregate/time.toml \
  -o generated-system
./generated-system/greenmeasurer \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -program fibonacci-30 -output system.csv \
  -n_times 1 -sleep-secs 0 -timeout-hours 0 --Time.WallClock --System.Cores
```

Generation does not run the workload. Hardware access may require additional
permissions; a powercap run can change system power limits. The output CSV and
calibration paths are relative to the executable's working directory. Header and
library paths are relative to each TOML file. The output CSV may be overwritten.
If an aspect cannot be built, the weaver reports why and omits its flags; use
`--help` to see which metrics were incorporated.

Measurement executables accept `--help` for available options. Select each
metric explicitly using its flag, or use `--no-metrics`. With no active
aspects, use `-command <cmd>` (optionally with `-n_times <N>`) to run the
command without a CSV or timeout. When a
selected aspect fails setup, the runtime warns and continues without it; if no
aspects remain, it runs the command once. Explicit calibration still fails if
its required aspect is unavailable. The measurement mode (`aggregate` or
`over-time`) is declared in the aspect TOML and woven into the executable;
the remaining measurement options are passed at runtime. Temperature metrics
and thermal control are independent: use `-temperature-control METRIC` with
that metric's `-calib-file` and `-variance` to control a measurement. An
unavailable controlled temperature disables its thermal control and repetition
pauses with a warning; other available controlled temperatures remain active.
Select a powercap control aspect such as `--IntelCPU.Powercap` explicitly to
apply powercap limits; it is independent of metric selection and produces no
metric column. `-powercap` is required only when a powercap control aspect is
selected.

Execution time is also an explicit aspect. The example declares
`Time.WallClock` and selects it above; without that flag its duration column is
not written. Over-time output always retains `Timestamp_ms` as its sampling axis.

See [docs/WEAVER.md](docs/WEAVER.md) for the configuration format, modes,
calibration, and CSV semantics. Per-aspect examples for both modes live in
[examples](examples) (`examples/aggregate/` and `examples/over-time/`). They
are hardware-specific: adapt them to the host and available libraries.
[configurations-paper](configurations-paper) reproduces the paper's usage
example (temperature calibration, powercap calibration, aggregate and
over-time) with its own aspect files.

To run the Fibonacci modes sequentially, see the
[Fibonacci sequence](scripts/README.md#fibonacci-sequence).

## Tests and utilities

```sh
make -C src test
make -C src clean
```

The tests use mock devices and do not require access to RAPL hardware. See
[scripts/README.md](scripts/README.md) for batch execution and analysis tools.
Archived experiment data is retained under [paper_data](paper_data).
