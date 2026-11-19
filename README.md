# GreenMeasurer

GreenMeasurer generates a Linux measurement executable from a TOML configuration.
The configuration explicitly declares the devices, metrics, and functions to use;
no metrics are selected automatically. The generator links the declared source files
or libraries with a runtime that executes the workload and writes CSV results.

| Authors | Contacts |
|---------|----------|
| Simão Cunha | simao.s.cunha@inesctec.pt |
| João Saraiva | saraiva@di.uminho.pt |

## Requirements

- Python 3.11 or Python 3 with `tomli`
- A C compiler, `make`, and POSIX threads
- Any headers, libraries, hardware access, and permissions required by the
  devices declared in the configuration

Optional example device libraries live in [src/devices](src/devices). Build
the ones whose dependencies are installed with:

```sh
make -C src
```

The Intel CPU example needs the powercap, raplcap, and libsensors development
files; the NVIDIA example needs NVML headers and its runtime library. Missing
optional development headers cause the affected library to be skipped.

## Generate and run

For example, after building the device libraries and checking that the host
supports the configured Intel devices:

```sh
python3 scripts/weave.py configurations-generated/fibonacci-python-standard.toml
./configurations-generated/generated-fibonacci-python-standard/greenmeasurer
```

Generation does not run the workload. Hardware access may require additional
permissions; a powercap run can change system power limits. The output CSV and
calibration paths are relative to the executable's working directory. Header and
library paths are relative to the TOML file. The output CSV may be overwritten.

Measurement executables accept `--help` for available options. With no metric
flags they use the metrics declared in the TOML; pass specific `--Device.Metric`
flags to select a subset, or `--no-metrics` to run without collecting metrics.
Thermal control and powercap hooks are independent of metric selection.
Configured powercap-calibration executables do not accept command-line overrides.

See [docs/WEAVER.md](docs/WEAVER.md) for the configuration format, modes,
calibration, and CSV semantics. Example configurations are in
[configurations-generated](configurations-generated). They are hardware-specific:
adapt them to the host and available libraries. For shorter, adaptable examples
by device or with all included readers together, see [examples](examples).

To run the four Fibonacci configurations sequentially, see the
[Fibonacci sequence](scripts/README.md#fibonacci-sequence).

## Tests and utilities

```sh
make -C src test
make -C src clean
```

The tests use mock devices and do not require access to RAPL hardware. See
[scripts/README.md](scripts/README.md) for batch execution and analysis tools.
Archived experiment data is retained under [paper_data](paper_data).
