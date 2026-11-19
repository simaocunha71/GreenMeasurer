# Scripts

Python 3.11 or `tomli` is required for TOML parsing.

- `weave.py`: generates and compiles an executable from an aspect-based TOML.
- `measure.sh`: generates a batch and runs it separately.
- `run_powercap_calibration.sh`: generates a powercap calibration executable.
- `run_all_configurations.sh`: builds and runs a set of configurations.
- `test_examples.sh`: builds and runs every TOML under `examples/`.
- `run_fibonacci_sequence.sh`: generates and runs the four Fibonacci configurations in order.
- `compare_overtime_overhead.py`: compares workload wall time with and without sampling.
- `compare_standard_overhead.py`: compares wall time with progressively more metrics.
- `analyze_overtime.py`: reports observed over-time sampling intervals.
- `probe_rapl_frequency.py`: observes Intel RAPL counter changes.
- `install.sh`: installs build and optional device dependencies.

## Batch generation and execution

```sh
bash scripts/measure.sh generate generated-batch first.toml second.toml
bash scripts/measure.sh run generated-batch
```

The batch directory must not already exist. Configurations execute in the order
supplied; use different output paths to avoid overwriting results. Generation
does not run workloads. An incomplete batch cannot be run.

For powercap calibration, generate with
`bash scripts/run_powercap_calibration.sh config.toml generated-powercap`, then
run `generated-powercap/greenmeasurer` with the necessary hardware permissions.
The TOML must set `measurement.mode = "powercap-calibration"` and declare the
calibration hook and range.

## Check all examples

```sh
bash scripts/test_examples.sh
```

The script recursively runs all example TOMLs and reports `PASS`, `WARN`, or
`FAIL`. A warning means the run completed but a log contains a warning or a
CSV contains `NOT_SUPPORTED`; inspect the per-configuration logs and CSVs in
the results directory printed at the end. Failures do not stop the remaining
runs; the script exits non-zero if any configuration fails. Use `-o DIR` to
choose the results directory or `-s` to stop at the first failure.

Runs use the TOML settings and require the declared libraries, hardware and
permissions. Powercap examples change CPU limits; review their configured
ranges before running. Workload and output paths run in separate results
directories; each example calibrates its own missing temperature baseline.

## Fibonacci sequence

```sh
bash scripts/run_fibonacci_sequence.sh [new-build-directory]
```

The script builds the optional device libraries, generates all four configured
executables, then runs temperature calibration, standard, over-time and powercap
calibration in that order. It stops on the first failure. The default build
directory is `generated-fibonacci-sequence`; the chosen directory must not
already exist. Commands and output paths run from the repository root.

The temperature configuration waits 60 seconds before taking one reading and
overwrites `cores_temperatures.txt`, which the other three configurations name
explicitly in their TOMLs. Measurements use the existing TOML settings and can
take a long time.
Standard and over-time measurements append to their CSVs; powercap calibration
may replace its output. Save existing results first, and use fresh CSVs after
changing the metric schema. The three measurement configurations declare CPU
Package, Core and DRAM energy, NVIDIA GPU energy and temperature, and Intel GPU
energy. Both GPUs can be recorded in the same run. GPU setup failures produce a
warning and `NOT_SUPPORTED` in the affected columns; individual failed readings
also produce `NOT_SUPPORTED`. The Intel CPU must support the configured thermal
and powercap operations. All declared libraries must be present for generation,
including the NVIDIA library and NVML development/runtime files, even when a GPU
is unavailable. Hardware access needs appropriate permissions; powercap runs
change CPU power limits. The powercap post-processor requires valid CPU Package
readings; if these are unavailable, it reports an error after the CSV is written.

## Overhead and timing analysis

```sh
python3 scripts/compare_standard_overhead.py --config standard.toml \
  --metrics System.Cores IntelCPU.Package --runs 10
python3 scripts/compare_overtime_overhead.py --config overtime.toml --runs 5
python3 scripts/analyze_overtime.py measurements.csv --threshold-us 1000
```

Metric names must be declared in the standard configuration. Comparisons run
the workload multiple times in randomized order and measure wall time, not
energy overhead. The scripts use temporary output files, preserving the
configured CSV. If a thermal hook is configured, its calibration file must
already exist. Standard comparisons apply powercap `-1`, while over-time
comparisons require that setting in the TOML. System power limits may still
apply; thermal waiting and other activity affect timing.

The timing analyzer treats changes in language, program or powercap, or a
non-increasing timestamp, as run boundaries. Identical runs with a single
sample may be indistinguishable. RAPL probing requires Linux MSR access and
its own polling changes the system load.
