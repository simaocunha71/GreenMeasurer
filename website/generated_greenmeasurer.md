# Generated GreenMeasurer

The weaver builds an executable from one or more TOML aspect files. Each file
declares at most one aspect through a `[measurement]` table, a `[device]` table
and `[[metric]]` entries. A metric aspect groups the metrics of one
energy-monitoring library; each metric supplies one possible CSV column. A
powercap control aspect supplies hooks without metrics. No metrics are
pre-included. The old `[[aspect]]` format is rejected.

```sh
python3 scripts/weave.py available-aspects/aggregate/system.toml available-aspects/aggregate/time.toml \
  -o generated-system
./generated-system/greenmeasurer --help
./generated-system/greenmeasurer \
  -command "python3 paper-experiments/fibonacci.py 30" \
  -program fibonacci-30 -output system.csv \
  -n_times 1 -sleep-secs 0 -timeout-hours 0 \
  --Time.WallClock --System.Cores
```

The weaver warns and skips an aspect if its header or library is missing, or if
it cannot be compiled or linked. The generated executable contains only the
aspects that were built and selects only the metrics named by flags. Use
`--help` to see available flags. A configuration with invalid syntax or aspect
fields, or a failure in the compiler or shared runtime, still fails generation.
The measurement mode is declared in the TOML and woven into the executable; the
workload and remaining execution options are supplied at invocation.
`--no-metrics` selects no metrics and may be combined with a control aspect flag.
With no selected or controlled aspects, `-command <cmd>` runs the command once.
The time aspect is declared like any other aspect and is not included unless
selected. In over-time, `Timestamp_ms` remains the mandatory sampling axis.

## Aspect declarations

One aspect per file. For example, `available-aspects/aggregate/intel-cpu.toml` declares
the Intel CPU energy aspect with three metrics:

```toml
[measurement]
aspect = "IntelCPU"
mode = "aggregate"
setup = "int intel_cpu_begin()"
teardown = "void intel_cpu_end()"

[device]
lib = "../../src/devices/libgm_intel_cpu.a"
header = "../../src/devices/linux/cpu/intel/intel_cpu.h"

[[metric]]
name = "IntelCPU.Package"
measurement = "double intel_cpu_get_package()"
kind = "counter"
unit = "J"

[[metric]]
name = "IntelCPU.Core"
measurement = "double intel_cpu_get_core()"
kind = "counter"
unit = "J"

[[metric]]
name = "IntelCPU.DRAM"
measurement = "double intel_cpu_get_dram()"
kind = "counter"
unit = "J"
```

A temperature aspect marks its metric with `thermal = true`, and a powercap
control aspect declares only hooks:

```toml
[measurement]
aspect = "IntelCPU.Powercap"
teardown = "void intel_cpu_powercap_end()"
powercap_set = "int intel_cpu_set_powercap(int)"
powercap_restore = "int intel_cpu_destroy_powercap()"
powercap_calibration = "intel_cpu_calibrate_powercap"

[device]
lib = "../../src/devices/libgm_intel_cpu_powercap.a"
header = "../../src/devices/linux/cpu/intel/intel_cpu_powercap.h"
link = ["-L/usr/local/lib", "-lraplcap-powercap", "-lpowercap"]
```

The `[device]` paths are relative to the TOML file. `measurement.aspect` is a
unique identifier whose parts are C identifiers separated by dots; it names the
generated module (for example `IntelCPU.c`) and, for a control aspect, its flag.
Each `[[metric]]` name follows the same rules, must be unique across all woven
files, and produces the flag `--IntelCPU.Package` and column
`IntelCPU.Package_J`. Names must not collide after dots are replaced by
underscores in generated C identifiers. `help` is a reserved flag name; at most
64 aspects can be declared, each with at most 64 metrics.

`measurement.mode` is `"aggregate"` or `"over-time"`. It is required for metric
aspects, optional for control aspects, and all woven files must agree; the mode
is fixed in the generated executable. In over-time, the weaver generates one
C module per aspect with a sampling thread that samples its selected metrics;
in aggregate, no additional C module is generated: every aspect is woven
directly into the main module and its metrics are read at the workload
boundaries.

`device.header` is required; each metric requires a no-argument numeric
`measurement` signature. `device.lib` can name a `.c`, `.o`, `.a` or `.so`;
`device.link` lists additional `-l` or `-L` flags. `kind` is `counter`, `gauge`
(default), or `duration`; `unit` is an optional column suffix. `setup` and
`teardown` are optional no-argument `void` or `int` functions in `[measurement]`.
A nonzero setup result disables the aspect. Aspects sharing a setup
function must declare the same teardown and share one initialization and
teardown. If setup fails, the runtime warns and continues without that aspect:
its metric columns are omitted, and its controls are not applied. Explicit
temperature or powercap calibration fails if its required aspect is unavailable.
`--no-build` skips aspects with missing files but cannot detect compilation or
linking failures; its printed command must still be built to verify those.

For elapsed time, declare a `duration` metric in an aspect with paired
no-argument `measurement_start` and `measurement_stop` callbacks (returning
`int` or `void`) in its `[measurement]` table. The runtime calls them
immediately before starting the workload and after it finishes. The metric's
`measurement` function returns the elapsed duration: the final value in
aggregate, and the elapsed value at each over-time sample. For example,
`available-aspects/aggregate/time.toml` declares `Time.WallClock`, implemented by
`src/aspects/wall_clock_time.c` using the same `CLOCK_MONOTONIC` calculation
previously used by the runtime. It is an optional example, not a preselected
metric. Alternative implementations can provide their own callbacks and reader.

Declare `thermal = true` on every temperature metric; at most one per aspect.
Its `measurement` function must return a temperature in degrees Celsius and its
kind must be `gauge`. Thermal metrics also require a `calibration` function
provided by the sensor library, declared as
`calibration = "double intel_cpu_calibrate_temperature(unsigned)"`: it receives
the stabilization time in seconds, sleeps, and returns the baseline
temperature, which the runtime stores in the calibration file.
The Intel temperature reader uses its own library and `-lsensors`; Intel energy
aspects do not require or initialize lm-sensors.

Powercap hooks belong to a separate control aspect with no metrics or
measurement callbacks. `powercap_set = "int set(int)"`
and `powercap_restore = "int restore()"` must be declared together;
`powercap_calibration` names a function matching `GmCalibratePowercapFn` in
`src/include/gm_aspect.h`. Declare at least one of these capabilities. A selected
control aspect produces no metric column or sampling thread. A control whose
setup fails is skipped with a warning and no limit is applied; an error while
applying or restoring a limit still aborts the measurement.
The Intel CPU readers and powercap control are separate libraries: a TOML without
the powercap aspect needs neither the powercap implementation nor its libraries.
A successfully built powercap aspect is linked into the executable but does
nothing unless selected at invocation.

## Modes and arguments

The measurement mode (`aggregate` or `over-time`) is woven into the executable
from the TOML. Calibration actions remain runtime flags:
`--temperature-calibration` or `--powercap-calibration`. Use `--help` to list
the flags available in the generated executable. For measurements, explicitly
provide `-command`, `-program`, `-output`, `-n_times`,
`-sleep-secs` and `-timeout-hours`; over-time executables also require
`-interval-ms`. Timeout is in hours; `0` means no timeout. Powercap is a
comma-separated list such as `-powercap "-1,5"`. Labels cannot contain CSV
commas, quotes or newlines.

Temperature calibration requires `-sleep-secs`, temperature metric flags and
one `-calib-file <metric>=<path>` for each selected temperature metric:

```sh
./greenmeasurer --temperature-calibration -sleep-secs 60 \
  --IntelCPU.Temperature -calib-file IntelCPU.Temperature=cpu-temperature.txt \
  --NvidiaGPU.Temperature -calib-file NvidiaGPU.Temperature=gpu-temperature.txt
```

Calibration overwrites the specified files. In measurements, selecting a
temperature metric records it but does not enable thermal control. Choose each
controlled sensor with `-temperature-control METRIC` (repeatable), its own
`-calib-file METRIC=PATH`, and `-variance` in degrees Celsius. A controlled
sensor need not be selected as a metric. For example, collect both temperatures
but control only the CPU:

```sh
./greenmeasurer -command "true" -program example \
  -output result.csv -n_times 1 -sleep-secs 0 -timeout-hours 0 \
  --IntelCPU.Temperature --NvidiaGPU.Temperature \
  -temperature-control IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=cpu-temperature.txt -variance 5
```

Before each run, all available controlled temperatures must be at most their own
baseline plus the variance. A missing file is calibrated first; an invalid file for an
available sensor fails the run. If a controlled sensor is unavailable, the runtime
warns and disables its thermal control for the rest of the execution. Other
available temperatures remain controlled. Automatic calibration waits are
skipped when any controlled temperature is unavailable, as is the over-time
pause between powercaps. There is no fixed pause between repetitions:
`-sleep-secs` is the settling time of temperature calibration, and thermal
control alone paces consecutive runs. Explicit temperature calibration fails
if a selected sensor is unavailable. Without thermal control, temperature metrics are
informational and need no calibration file or variance.

Select a declared control aspect explicitly, for example `--IntelCPU.Powercap`,
to apply the limits given by the required `-powercap` option in aggregate or
over-time. Without a selected control, omit `-powercap`: no limits are changed
and the CSV `Powercap` field is `-1`. Explicit `-powercap -1` remains accepted
in that case; positive limits without a selected control fail. At most one powercap control
aspect can be selected. To apply a limit without collecting any metrics, combine
`--no-metrics --IntelCPU.Powercap` with the usual execution arguments.

```sh
./greenmeasurer -command "true" -program example \
  -output result.csv -n_times 1 -sleep-secs 0 -powercap "-1,5" -timeout-hours 0 \
  --IntelCPU.Powercap --IntelCPU.Package
./greenmeasurer --powercap-calibration -range 5-15 -command "true" \
  -program example -output calibration.csv -n_times 1 \
  -sleep-secs 0 -timeout-hours 0 --IntelCPU.Powercap --IntelCPU.Package
```

Powercap calibration requires the control aspect flag, `-range MIN-MAX`, the command, labels, output,
repetition count, sleep, timeout and selected aspect flags. It uses the declared
calibration hook; optional `-compile_cmd` runs before the sweep. If temperature
control is enabled, provide the controlled sensors, calibration files and variance as above.
The supplied Intel hook tests `-1` and every integer in the requested range.
The automatic speedup/greenup report requires a Package energy metric and
exactly one selected `duration` aspect in milliseconds. If several are
selected, the report is skipped; use
`src/devices/linux/cpu/intel/processPowercapCalibrationCSV.py` with
`--time-column` to select the column explicitly. Besides printing the report,
the script records the powercap with the lowest package energy in
`<output>-optimal-powercap.txt`, next to the calibration CSV, so that later
measurements can reuse it; other limits can still be applied explicitly with
`-powercap`, backed by the control aspect's `powercap_set` hook.
**Powercap changes hardware limits** and may require privileges.

The Intel implementation stores the saved package/die RAPL limits in
`/var/lib/greenmeasurer/intel-rapl-baseline` on the first powercap operation,
if no saved file exists. It captures the current limits, not necessarily
factory or boot-time settings. `-1` applies that saved baseline when the
powercap control aspect is selected; otherwise, `-1` is only a CSV label.

## Execution and output

Aggregate mode uses no sampling threads. It executes the workload through a
system call; counters report the
end-minus-start difference and gauges the final reading. Over-time starts one
thread per aspect in its generated C file, sampling that aspect's selected
metrics; each counter row reports the consumption of that interval (the reading
taken at that instant minus the reading of the previous row), so summing a
run's rows yields the same end-minus-start total that aggregate mode reports.
Readers declared with the same header are serialized; readers from different
headers may run concurrently and their libraries must support that.
For every workload execution, including a new powercap, each sampling buffer
starts empty and counter deltas restart from that execution's first reading,
even though the underlying reader is never reset and may keep counting across
powercaps. The Intel RAPL readers return the hardware counter converted to
joules; its origin is hardware-defined and it wraps around when it overflows,
which makes the difference negative and reports `ERROR` in both modes. On the
common grid, a gauge reading can be repeated and any metric is unavailable
until the first sample arrives; a counter row without a new sample reports `0`.
Samples are aligned to a `Timestamp_ms` grid. `--max-samples` controls the
compile-time capacity (default 1,000,000); overflow fails the measurement.
The optional `Time.WallClock_ms` column is the elapsed wall-clock time measured
by the `Time.WallClock` aspect, including process startup and waiting; it is not
CPU time. `Timestamp_ms` is the output time axis, not another measurement of
workload duration. Its origin is recorded before the duration aspect starts,
and its final value can include processing or the last sample after that aspect
stops. Sampling and grid alignment can also produce differences or repeated
values. Therefore `Timestamp_ms` can be larger than `Time.WallClock_ms`, even
on the final row.
Unavailable readings are written as `ERROR`. Executions append rows to the
specified CSV file across repetitions, powercap levels, and calibration runs,
rejecting an existing header that does not match the selected aspects and mode.
Aggregate CSVs contain `Program,Powercap` and the selected aspect
columns; there is no implicit execution-time column.

With no selected aspects, or if all selected aspects fail setup, the executable
runs the given command with `system()` and returns its status; `-n_times`
optionally repeats it, stopping at the first failure. It produces no CSV and
does not enforce a timeout. The setup-failure fallback runs the command once. If measurement options
were provided, it warns that they will not be used. An empty TOML additionally
omits the measurement runtime from the generated executable:

```sh
python3 scripts/weave.py empty.toml -o generated-plain
./generated-plain/greenmeasurer -command "python3 prog.py"
```

The same command-only invocation also works when aspects are declared but none
are selected.