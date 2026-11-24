# Scripts

Python 3.11 or Python with `tomli` is required. The weaver and generated
executable are documented in [WEAVER.md](../docs/WEAVER.md).

## Directory contents

- `install.sh`: installs system and device dependencies (including `powercap`,
  `raplcap`, MSR/RAPL kernel modules, and optional NVIDIA NVML headers on
  Linux, or Homebrew build tools on macOS), sets up `.venv` with `pandas` and
  `tomli`, and builds the device libraries under `src/`.
- `weave.py`: validates aspect TOML files, generates C code for `aggregate` or
  `over-time` mode, skips aspects whose headers, libraries, or compilation
  fail, and compiles the `greenmeasurer` binary.
- `measure.sh`: generates and executes batches of `greenmeasurer` binaries.

## Weaving an executable directly

```sh
python3 scripts/weave.py available-aspects/aggregate/system.toml \
  available-aspects/aggregate/time.toml -o generated-system-time
```

Use `--max-samples <N>` to set the static sample capacity in `over-time` mode
(default: `1000000`)

## Batch generation and execution

```sh
bash scripts/measure.sh generate generated-batch first.toml second.toml
bash scripts/measure.sh run generated-batch -- \
  -command "true" -program true -output results.csv \
  -n_times 1 -sleep-secs 0 -timeout-hours 0 --System.Cores
```

Each `generate` argument builds one executable: a TOML file is woven alone and
a directory weaves all its `*.toml` aspect files together. The batch directory
must not already exist. Generation builds the device libraries and weaves the
executables without running workloads.

By default, `run` rebuilds each generated executable from its recorded `.toml`
inputs before executing it (`--no-build` skips rebuilding, `--build-devices`
also rebuilds device libraries, and `--max-samples <N>` sets the sample array
capacity). `run` passes the same explicit arguments to each executable in
order, so every selected flag must exist in each configuration and the woven
modes must accept the same options. Use compatible CSV schemas or run
experiments separately. An incomplete batch cannot be run.

## Related workflows

- Guided walkthrough: [quickstart/README.md](../quickstart/README.md)
- Available aspects and CLI flags:
  [available-aspects/README.md](../available-aspects/README.md)
- Paper experiment sequence:
  [paper-experiments/README.md](../paper-experiments/README.md)