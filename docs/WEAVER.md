# Generated GreenMeasurer

`scripts/weave.py` builds a GreenMeasurer executable from a TOML file in which the
user declares every aspect and metric. An aspect groups metric readers and optional
control hooks; it can use a device library or other user-provided code. Nothing is
pre-included: all metrics come from
existing `.c`/`.h`/`.a` code and the weaver only generates the glue.

```
python3 scripts/weave.py config.toml [-o out_dir] [--max-samples N]
```

By default, the generated executable is placed in `generated-<config name>`
next to the TOML file. Use `-o` to choose another output directory.

Every `[measurement]` table must declare a non-empty `calibration_file` path.
Modes that use thermal control read this file and create it if it is missing.

## TOML format

```toml
[measurement]
mode = "over-time"          # over-time | standard | temperature-calibration | powercap-calibration
command = "python3 prog.py"
language = "Python"
program = "prog"
output = "result.csv"
interval_ms = 1             # sampling period (over-time only)
repetitions = 3             # runs per powercap
sleep_seconds = 0           # pause between repetitions
timeout_hours = 1           # optional timeout per workload
powercap = [-1, 40]         # limits in W (-1: saved RAPL baseline); default -1
variance = 5                # thermal tolerance above the baseline, in C
calibration_file = "temperature-baseline.txt"     # required baseline path

[[aspect]]
name = "RAPL"
lib = "rapl.a"              # .a, .so, .o or .c; relative to the TOML file
header = "rapl.h"
link = ["-lnvidia-ml"]      # optional extra linker flags
optional = false            # true: retain columns as NOT_SUPPORTED if setup fails
setup = "void rapl_begin()"     # optional; "int" return: non-zero indicates failure
teardown = "void rapl_end()"    # optional
thermal = "double rapl_temp()"              # optional: temperature used for thermal control
powercap_set = "int rapl_set_cap(int)"      # optional, together with powercap_restore
powercap_restore = "int rapl_restore_cap()"
powercap_calibration = "rapl_calibrate"     # optional; fixed signature, see gm_aspect.h

[[aspect.metric]]
name = "Package"
read = "unsigned long rapl_package()"   # return type selects the array type
kind = "counter"            # counter | gauge (default)
unit = "uJ"                 # optional, appended to the CSV column
```

Aspects are `[[aspect]]` tables with `[[aspect.metric]]` entries. The old
`[[device]]` format is no longer accepted.

With no aspects, the generated executable calls `system(command)` once and returns
the command's exit status. It does not link the measurement runtime, generate a
CSV, measure time, enforce a timeout or accept command-line arguments. Only
`mode`, `calibration_file` and a non-empty `command` are accepted in this case;
the first two remain required for a valid TOML but do not cause measurement.

```toml
[measurement]
mode = "standard"
calibration_file = "unused-temperature-baseline.txt"
command = "python3 prog.py"
```

When a selected aspect has `optional = true` and its setup fails, a warning is
emitted, its hooks and readers are disabled, and its selected CSV columns contain
`NOT_SUPPORTED`. Other aspects continue normally. Required aspects still abort
on setup failure. This does not make the aspect's declared build dependencies
optional, and a required thermal or powercap operation can still fail if its
hook is unavailable.

For powercap calibration, `measurement.compile_command` can optionally compile the
workload before the sweep. The default timeout is one hour for calibration and
unlimited for other modes; `timeout_hours` overrides it.

For temperature calibration, set `mode = "temperature-calibration"`, declare a
aspect with a `thermal` hook, and set `sleep_seconds` and `calibration_file`.
The generated executable waits for that period, reads the temperature once and
overwrites the baseline file. No workload is run.

## Generated code

For each aspect, `<Aspect>.c` contains the sampling thread function, its
`pthread_create` and `pthread_join` calls, and one static array per metric
(`float gm_samples_NumberProcesses[GM_MAX_SAMPLES]`), filled with the user's function.
`GreenMeasurer.c` registers the aspects and adds one flag per metric
(`--Aspect.Metric`). Without metric flags the metrics of the TOML are measured;
`--no-metrics` measures none. `gm_runtime.c` (static code) runs the command and writes the CSV.

* Capacity is a compile-time constant (`--max-samples`, default 1,000,000); no allocation
  happens at run time. Exceeding it aborts the measurement.
* Over-time: the runtime invokes each active aspect's start/stop functions; those
  functions create and join one sampling thread in the generated `<Aspect>.c`.
  Rows share a common `Timestamp_ms` grid and each aspect contributes its latest
  sample within half an interval. The grid starts after the
  first interval, and a final row is retained for short workloads. Counters are raw values.
  Each powercap runs `repetitions` times, with `sleep_seconds` between repetitions.
* Standard: aspects remain active, but no sampling threads are created; counters report
  `end - start` and gauges the final reading. The runtime starts the workload with
  `fork()` and `/bin/sh -c` via `exec()`, retaining its PID for timeout and
  workload-scoped metrics.
* A negative reading means unavailable and is written as `NOT_SUPPORTED`.

## Powercap, thermal control and calibrations

These are optional hooks of an aspect, so the runtime stays free of any vendor code.

* `-powercap W[,W...]` (or `measurement.powercap`): every limit is applied with
  `powercap_set`, measured, and undone with `powercap_restore`. The `Powercap` CSV column holds
  the limit. `-1` applies the saved per-machine baseline. A positive limit needs an aspect with
  both hooks.
  The Intel CPU implementation stores its RAPL package/die long and short limits and
  enabled state in `/var/lib/greenmeasurer/intel-rapl-baseline`, under a lock. It
  loads that file when powercap is first used; if it does not exist, it captures
  and saves the then-current limits before changing them. This is not captured by
  ordinary aspect setup and is not necessarily a factory or boot-time default.
  `-1` restores that saved state, and teardown restores it after a capped run.
* Before each run the runtime waits until `thermal()` is at most the baseline plus `-variance`.
  The baseline is read from `-calib-file`; if the file does not exist it is calibrated first.
  Without a `thermal` hook there is no thermal control.
* `--temperature-calibration [-sleep-secs S] [-calib-file F]` stores the `sensors` snapshot and
  the average temperature.
* `--powercap-calibration -range MIN-MAX -command CMD [-compile_cmd CMD] [-n_times N]
  [-variance V] [-time_out_limit HOURS] [-output CSV]` calls the aspect's `powercap_calibration`
  function, which sweeps `-1` and `MIN..MAX` W and calls back `runTesting()` (provided by the
  runtime, standard mode). A Package summary is produced by `Utils/processPowercapCalibrationCSV.py`.
  The runtime waits `sleep_seconds` between repetitions at each powercap.
* Hook-only aspects (no selected metric) whose setup fails only lose their hooks, with a warning.

The optional example implementations in `src/devices/` are linked only when a
configuration explicitly declares them.
