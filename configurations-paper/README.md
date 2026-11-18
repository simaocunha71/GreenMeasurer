# Running the paper configurations

Run these commands from the repository root. Build GreenMeasurer once:

```bash
make -C src
```

## Temperature calibration

Run temperature calibration directly before the other configurations. It
creates `paper-temperature-baseline.txt` in the current directory:

```bash
./src/main --config configurations-paper/temperature-calibration.toml
```

## Powercap calibration

This configuration calibrates power caps from 1 W to 25 W. Generate its
configured executable, then run it with the permissions required by the
host's powercap interface:

```bash
./src/main \
  --config configurations-paper/powercap-calibration.toml \
  --directory generated-paper-powercap-calibration
sudo ./generated-paper-powercap-calibration/greenmeasurer
```

## Standard measurements

The standard configuration runs ten repetitions for each powercap setting:
no GreenMeasurer power cap (`-1`) and a 5 W cap. Both settings are written to
`paper-standard.csv`.

```bash
./src/main \
  --config configurations-paper/standard.toml \
  --directory generated-paper-standard
./generated-paper-standard/greenmeasurer
```

## Over-time measurements

The over-time configuration also measures both the uncapped and 5 W settings.
It requests a 1-millisecond pause between sampling cycles; actual sample
periods are longer because metric reads and scheduling take time. One
invocation runs once for each powercap setting.

```bash
./src/main \
  --config configurations-paper/overtime.toml \
  --directory generated-paper-overtime
./generated-paper-overtime/greenmeasurer
```

The standard and over-time configurations select all metrics. Metrics
unavailable on the host may be skipped with diagnostics. Powercap operations
require compatible hardware and may require elevated privileges.

Each generated executable embeds its TOML configuration and does not accept
runtime overrides. The output directory must not already exist; use a new
directory when generating again. Measurement CSVs are written to the paths
specified by each TOML, relative to the directory where the executable runs.
Successful runs overwrite those output files.
