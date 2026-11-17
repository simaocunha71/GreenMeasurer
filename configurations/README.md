# Running the example configurations

Run these commands from the repository root. Build GreenMeasurer once before
running the measurement configurations:

```bash
make -C src
```

## Calibration

Run temperature calibration first. It runs directly and creates the shared
`greenmeasurer-test-temperature.txt` baseline in the current directory:

```bash
./src/main --config configurations/selected-some-metrics/temperature.toml
```

Powercap calibration uses that baseline. Generate its configured executable,
then run it with the permissions required by the host's powercap interface:

```bash
./src/main \
  --config configurations/selected-some-metrics/powercap-calibration.toml \
  --directory generated-selected-powercap-calibration
sudo ./generated-selected-powercap-calibration/greenmeasurer
```

## Selected metrics

These configurations select a small set of system and CPU metrics. Both use
`powercap = -1`, so GreenMeasurer applies the saved per-machine RAPL baseline.
The first Intel run captures the current hardware limits; verify they are the
desired baseline before running. Applying or saving the baseline may require
elevated privileges.

```bash
./src/main \
  --config configurations/selected-some-metrics/standard.toml \
  --directory generated-selected-standard
sudo ./generated-selected-standard/greenmeasurer

./src/main \
  --config configurations/selected-some-metrics/overtime.toml \
  --directory generated-selected-overtime
sudo ./generated-selected-overtime/greenmeasurer
```

## All metrics

These run standard and over-time measurements with `select = "all"`. Metrics
that are unavailable on the host may be skipped with diagnostics.

```bash
./src/main \
  --config configurations/all-metrics/standard.toml \
  --directory generated-all-metrics-standard
sudo ./generated-all-metrics-standard/greenmeasurer

./src/main \
  --config configurations/all-metrics/overtime.toml \
  --directory generated-all-metrics-overtime
sudo ./generated-all-metrics-overtime/greenmeasurer
```

## User-defined uptime metric

These configurations compile the external metric from
`new-metrics/system_uptime.c` and `new-metrics/system_uptime.h` when generating
the executable.

```bash
./src/main \
  --config configurations/new-metrics/standard.toml \
  --directory generated-new-metric-standard
sudo ./generated-new-metric-standard/greenmeasurer

./src/main \
  --config configurations/new-metrics/overtime.toml \
  --directory generated-new-metric-overtime
sudo ./generated-new-metric-overtime/greenmeasurer
```

Each generated executable embeds its TOML configuration and does not accept
runtime overrides. The output directory must not already exist; use a new
directory when generating again. Measurement CSVs are written to the paths
specified by each TOML, relative to the directory where the executable runs.
Those output files are overwritten on a successful run. Hardware-dependent
metrics and powercap operations may require compatible hardware and elevated
permissions. On Intel, `powercap = -1` applies the saved per-machine baseline.
