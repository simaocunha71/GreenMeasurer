# Progressive Fibonacci example

Run from the repository root:

```sh
bash toy-examples/run.sh
```

If elevated permissions are needed, `bash toy-examples/run.sh --sudo-powercap`
uses `sudo -A` for the sixth selection's powercap operations only.

The script generates one executable and runs recursive Python Fibonacci 20 with
six successive selections: no aspects; wall-clock time; CPU temperature; Intel
CPU package energy; NVIDIA GPU temperature; and Intel CPU powercap. Each selection retains the
previous aspects.

Each selection runs aggregate and over-time once, with a 1 ms sampling interval
for over-time and a 10-second sleep setting. Without active aspects, the
runtime executes the command once directly and does not produce a CSV. Selected
temperature aspects are calibrated separately with 10 seconds of stabilization;
after successful CPU calibration, measurement uses CPU thermal control with a
15 C variance. NVIDIA temperature is measured but not used for thermal control.
This short calibration is only for checking behaviour, not a controlled experiment.
With one repetition per powercap, there is no sleep between repetitions.

The sixth selection calibrates powercap at the saved baseline and 5–7 W, then
uses the saved baseline (`-1`) and 5 W for aggregate and over-time. The saved
baseline is not necessarily uncapped. **This changes real CPU power limits** and
requires appropriate permissions; the backend restores its saved baseline, which
may differ from the limits in effect before the run. Check both before running.
If calibration or aggregate fails, the script does not attempt further powercap
operations. Powercap calibration is skipped for the other selections.

Each invocation creates a
new `results-*` directory containing build logs, available and requested
aspects, per-mode logs, calibration files and measurement CSVs. Both stdout and
stderr from the workload and GreenMeasurer go into the mode logs. Logs may add
overhead.

Hardware and permissions determine which aspects can be built and started. A
missing build-time aspect is recorded and omitted from subsequent commands; a
runtime setup failure is reported by GreenMeasurer in the relevant log. The
script continues other cases and exits nonzero if an aspect could not be built
or a command failed. Inspect the logs and CSV headers to distinguish the
requested aspects from those actually used.
