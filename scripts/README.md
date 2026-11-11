### Scripts

- `install.sh`: Installs build and telemetry dependencies. Linux is the currently supported platform; installing tools on another OS does not provide a GreenMeasurer port.
- `measure.sh`: Generates a batch from TOML files, then runs it in a separate step.
- `run_powercap_calibration.sh`: Generates a powercap calibration executable from TOML.
- `analyze_overtime.py`: Reports observed over-time sampling intervals.
- `probe_rapl_frequency.py`: Measures observed Intel package RAPL counter changes.
- `compare_overtime_overhead.py`: Compares workload wall time with and without over-time sampling.
- `validate_config.py`: Validates the TOML configuration. Optional
  `--check-adapters` loads trusted selected adapters and tests their readings.
- `run_config.py`: Internal configuration-to-engine argument translation.
- `generate_config.py`: C generation and compilation, including
  source-based external metrics. It does not execute the workload; see
  the [main README](../README.md#executing-a-configuration).

Python 3.11+ or `tomli` is required for TOML parsing.

## Batch generation and execution

```sh
bash scripts/measure.sh generate generated-batch first.toml second.toml
bash scripts/measure.sh run generated-batch
```

The batch directory must not already exist. Configurations execute in the
order supplied. Each TOML specifies its own command, metrics and output;
use different output paths to avoid overwriting results. Generation does not
run workloads. On a generation failure, fix the configuration and select a
new directory; an incomplete batch must not be run. Unlike the old script,
this does not alter Wi-Fi, clean builds, or delete CSV files.

For powercap calibration, generate with
`bash scripts/run_powercap_calibration.sh config.toml generated-powercap`
and then run `generated-powercap/greenmeasurer` separately with the necessary
hardware permissions.

## Configuration and backend diagnostics

Both modes support explicitly selected CPU, GPU, system and external metrics.
`select = "all"` includes all system metrics, runtime-selected CPU/GPU backend
metrics, all metrics in declared adapters, and all declared source metrics.
Unavailable metrics are skipped with diagnostics.
Runnable examples and details are in the [main README](../README.md#executing-a-configuration).

Default backend output shows selected plugins and load/descriptor errors; it
warns when CPU or GPU has no active backend. Undetected, unbuilt and
lower-priority backends are hidden. For full diagnostics, including permission
hints, set `GREENMEASURER_VERBOSE_BACKENDS=1`, for example:

```sh
./src/main --config src/tests/fixtures/configurations/system-cores.toml --directory generated-diagnostics
GREENMEASURER_VERBOSE_BACKENDS=1 ./generated-diagnostics/greenmeasurer
```

The command runs a measurement and overwrites the example's output CSV in the
current directory. It does not merely list backends.

## Evaluating over-time sampling

These scripts use Python 3's standard library. Run them from the repository root.

1. Analyze the actual sample timestamps in an existing CSV:

   ```sh
   python3 scripts/analyze_overtime.py measurements_ot.csv --threshold-ms 1
   ```

   For each inferred run this prints sample count, mean, median, minimum, 95th
   percentile (linear interpolation), maximum, and count and percentage of
   intervals **strictly greater** than the threshold. Only differences between
   consecutive timestamps are included; the first window has no preceding
   timestamp, while the last interval can include a shorter final window.
   The CSV has no run identifier: a change in language, program or powercap, or
   a non-increasing timestamp, is interpreted as a new run. Identical runs can
   be indistinguishable, particularly short runs with a single sample. Keep
   separate per-run CSV files for reliable identification. The older `Time_ms`
   format does not contain timestamps and cannot be analyzed this way.

2. Observe counter changes on an Intel machine:

   ```sh
   sudo python3 scripts/probe_rapl_frequency.py --duration 5 --core 0
   ```

   This continuously polls `MSR_PKG_ENERGY_STATUS`, using `/dev/cpu/0/msr`,
   and reports observed changes and their intervals. The module `msr` and
   permission to read the device are required. The initial interval is from
   the first read to the first detected change and can be partial. Polling
   consumes CPU and can affect energy and scheduling; observed changes do not
   prove every hardware update was seen or reveal the exact hardware update
   time. Repeat under the conditions being studied and record the hardware,
   operating system, workload, and probe settings. This standalone probe measures
   the counter under its own polling load, not simultaneously with the workload.

3. Compare wall-clock times for the same workload with and without over-time sampling:

   ```sh
   python3 scripts/compare_overtime_overhead.py \
     --config path/to/overtime.toml --runs 5
   ```

   The workload runs twice per pair, in reproducibly randomized order. The
   TOML supplies the command, selected metrics, sampling interval and other
   settings. An existing calibration file is required (using
   `measurement.calibration_file` or the default `/tmp/cores_temperatures.txt`);
   prepare it separately using a temperature-calibration TOML if necessary. The
   script generates a temporary configured executable once and redirects its
   output to a temporary CSV, without changing `measurement.output` in the
   file or overwriting its configured CSV. Source paths and adapter paths are
   resolved relative to the original configuration. Both modes run under the same
   user as the script. The baseline uses shell elapsed time; over-time workload
   time is approximated by its final sample timestamp, and `wrapper_ms` also
   includes initialization, possible temperature waiting and CSV output.
   The script prints every run in execution order and the difference in mean
   workload time. It uses temporary CSVs and does not change `measurements_ot.csv`.
   The command must be safe to repeat and run in the foreground. This is a
   comparison of **wall time**, not energy overhead, and does not isolate
   sampling overhead from noise, warm-up, thermal effects or scheduler effects.
   An independent energy meter or another separately validated method is
   needed to quantify energy overhead.

   Redirect standard output to a file to retain the run order, individual
   timings and summary, for example append `> overhead-report.txt`. Save
   standard error separately when retaining warnings or failure diagnostics.
   The comparison requires the TOML's powercap to be absent or `-1`, because
   the unmeasured baseline cannot apply a GreenMeasurer cap. This does not
   remove a cap previously applied outside this run; verify the system's
   actual power limits before comparing.

Over-time sampling reads the backends and system metrics sequentially. `Timestamp_ms` is
captured before the backend reads and is not their exact simultaneous read time.
The configured 1 ms pause begins after completing a cycle, so it is not a
guaranteed 1 ms period. RAPL window values are interval energy, not
instantaneous power; dividing interval energy by the difference in timestamps
estimates average power with timing uncertainty. The first interval's baseline
precedes the recorded start timestamp. The final window can extend slightly
beyond workload completion. Package energy includes unrelated system activity.
Save raw results, repeat experiments, report variation and document conditions;
the scripts' summaries alone do not establish statistical significance.
