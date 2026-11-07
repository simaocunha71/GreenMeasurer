### Scripts

- `install.sh`: Installs all prerequisites for the study, such as `git`, `cmake`, `lm-sensors`, etc.
- `measure.sh`: Executes experiments on programs.
- `run_powercap_calibration.sh`: Runs powercap calibration.
- `analyze_overtime.py`: Reports observed overtime sampling intervals.
- `probe_rapl_frequency.py`: Measures observed Intel package RAPL counter changes.
- `compare_overtime_overhead.py`: Compares workload wall time with and without overtime sampling.

## Evaluating overtime sampling

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

3. Compare wall-clock times for the same workload with and without overtime:

   ```sh
   make -C src
   sudo python3 scripts/compare_overtime_overhead.py \
     --command 'sleep 2' --calib-file /tmp/cores_temperatures.txt \
     --runs 5 --interval-us 1000
   ```

   The workload runs twice per pair, in reproducibly randomized order. The
   existing calibration file is required; prepare it separately with
   `--temperature-calibration` if necessary. Both modes run under the same
   user as the script. The baseline uses shell elapsed time; overtime workload
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
   The `-1` powercap setting does not remove a cap previously applied outside
   this run; verify the system's actual power limits before comparing.

Overtime reads the backends and system metrics sequentially. `Timestamp_ms` is
captured before the backend reads and is not their exact simultaneous read time.
The configured 1 ms pause begins after completing a cycle, so it is not a
guaranteed 1 ms period. RAPL window values are interval energy, not
instantaneous power; dividing interval energy by the difference in timestamps
estimates average power with timing uncertainty. The first interval's baseline
precedes the recorded start timestamp. The final window can extend slightly
beyond workload completion. Package energy includes unrelated system activity.
Save raw results, repeat experiments, report variation and document conditions;
the scripts' summaries alone do not establish statistical significance.
