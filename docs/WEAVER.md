# Generated GreenMeasurer

The weaver builds an executable from a TOML file containing only `[[aspect]]`
declarations. A metric aspect supplies one possible CSV column; a powercap
control aspect supplies hooks without a metric. No metrics are pre-included.
The old `[measurement]` and `[[aspect.metric]]` formats are rejected.

```sh
python3 scripts/weave.py examples/system/config.toml -o generated-system
./generated-system/greenmeasurer --help
./generated-system/greenmeasurer --aggregate \
  -command "python3 Languages/Python/Fibonacci/fibonacci.py 30" \
  -language Python -program fibonacci-30 -output system.csv \
  -n_times 1 -sleep-secs 0 -time_out_limit 0 \
  --Time.WallClock --System.Cores
```

The weaver warns and skips an aspect if its header or library is missing, or if
it cannot be compiled or linked. The generated executable contains only the
aspects that were built and selects only those named by flags. Use `--help` to
see available flags. A configuration with invalid syntax or aspect fields, or
a failure in the compiler or shared runtime, still fails generation.
Mode and execution options are supplied at invocation; no
workload or measurement options are embedded in the TOML or executable.
`--no-metrics` selects no metrics and may be combined with a control aspect flag.
With no selected or controlled aspects, `-command <cmd>` runs the command once.
The time aspect is declared like any other aspect and is not included unless
selected. In over-time, `Timestamp_ms` remains the mandatory sampling axis.

## Aspect declarations

```toml
[[aspect]]
name = "IntelCPU.Package"
lib = "../src/devices/libgm_intel_cpu.a"
header = "../src/devices/linux/cpu/intel/intel_cpu.h"
setup = "int intel_cpu_begin()"
teardown = "void intel_cpu_end()"
read = "double intel_cpu_get_package()"
kind = "counter"
unit = "J"

[[aspect]]
name = "IntelCPU.Temperature"
lib = "../src/devices/libgm_intel_cpu_temperature.a"
header = "../src/devices/linux/cpu/intel/intel_cpu_temperature.h"
link = ["-lsensors"]
setup = "int intel_cpu_temperature_begin()"
teardown = "void intel_cpu_temperature_end()"
read = "double intel_cpu_get_temperature()"
kind = "gauge"
unit = "C"
thermal = true

[[aspect]]
name = "IntelCPU.Powercap"
lib = "../src/devices/libgm_intel_cpu_powercap.a"
header = "../src/devices/linux/cpu/intel/intel_cpu_powercap.h"
link = ["-L/usr/local/lib", "-lraplcap-powercap", "-lpowercap"]
teardown = "void intel_cpu_powercap_end()"
powercap_set = "int intel_cpu_set_powercap(int)"
powercap_restore = "int intel_cpu_destroy_powercap()"
powercap_calibration = "intel_cpu_calibrate_powercap"
```

The paths above are relative to the TOML file. `name` is a unique identifier
whose parts are C identifiers separated by dots; it produces the flag
`--IntelCPU.Package` and column `IntelCPU.Package_J`. Names must not collide
after dots are replaced by underscores in generated C identifiers. `help` and
`aggregate` are reserved flag names; at most 64 aspects can be declared.

`header` is required; metric aspects also require a no-argument numeric `read`
signature. `lib` can
name a `.c`, `.o`, `.a` or `.so`; `link` lists additional `-l` or
`-L` flags. `kind` is `counter`, `gauge` (default), or `duration`; `unit` is an optional
column suffix. `setup` and `teardown` are optional no-argument `void` or `int`
functions. A nonzero setup result disables the aspect. Aspects sharing a setup
function must declare the same teardown and share one initialization and
teardown. If setup fails, the runtime warns and continues without those aspects:
their metric columns are omitted, and their controls are not applied. Explicit
temperature or powercap calibration fails if its required aspect is unavailable.
`--no-build` skips aspects with missing files but cannot detect compilation or
linking failures; its printed command must still be built to verify those.

For elapsed time, declare a `duration` aspect with paired no-argument
`measurement_start` and `measurement_stop` callbacks returning `int` or
`void`. The runtime calls them immediately before starting the workload and
after it finishes. Its `read` function returns the elapsed duration: the final
value in aggregate, and the elapsed value at each over-time sample. For
example, `configurations-generated/config.toml` declares
`Time.WallClock`, implemented by `src/aspects/wall_clock_time.c` using the
same `CLOCK_MONOTONIC` calculation previously used by the runtime. It is an
optional example, not a preselected metric. Alternative implementations can
provide their own callbacks and reader.

Declare `thermal = true` on every temperature aspect. Its `read` function must
return a temperature in degrees Celsius and its kind must be `gauge`.
The Intel temperature reader uses its own library and `-lsensors`; Intel energy
aspects do not require or initialize lm-sensors.

Powercap hooks belong to a separate control aspect with no `read`, `kind`,
`unit`, temperature or measurement callbacks. `powercap_set = "int set(int)"`
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

For measurement or calibration, exactly one mode is required: `--aggregate`, `--over-time`,
`--temperature-calibration`, or `--powercap-calibration`. Use `--help` to list
the flags available in the generated executable. For aggregate and over-time,
explicitly provide `-command`, `-language`, `-program`, `-output`, `-n_times`,
`-sleep-secs` and `-time_out_limit`; over-time also requires
`-interval-ms`. Timeout is in hours; `0` means no timeout. Powercap is a
comma-separated list such as `-powercap "-1,5"`. Labels cannot contain CSV
commas, quotes or newlines.

Temperature calibration requires `-sleep-secs`, temperature aspect flags and
one `-calib-file <aspect>=<path>` for each selected temperature aspect:

```sh
./greenmeasurer --temperature-calibration -sleep-secs 60 \
  --IntelCPU.Temperature -calib-file IntelCPU.Temperature=cpu-temperature.txt \
  --NvidiaGPU.Temperature -calib-file NvidiaGPU.Temperature=gpu-temperature.txt
```

Calibration overwrites the specified files. In measurements, selecting a
temperature aspect records it but does not enable thermal control. Choose each
controlled sensor with `-temperature-control ASPECT` (repeatable), its own
`-calib-file ASPECT=PATH`, and `-variance` in degrees Celsius. A controlled
sensor need not be selected as a metric. For example, collect both temperatures
but control only the CPU:

```sh
./greenmeasurer --aggregate -command "true" -language Shell -program example \
  -output result.csv -n_times 1 -sleep-secs 0 -time_out_limit 0 \
  --IntelCPU.Temperature --NvidiaGPU.Temperature \
  -temperature-control IntelCPU.Temperature \
  -calib-file IntelCPU.Temperature=cpu-temperature.txt -variance 5
```

Before each run, all available controlled temperatures must be at most their own
baseline plus the variance. A missing file is calibrated first; an invalid file for an
available sensor fails the run. If a controlled sensor is unavailable, the runtime
warns and disables its thermal control for the rest of the execution. Other
available temperatures remain controlled. Automatic calibration waits and
repetition pauses are skipped when any controlled temperature is unavailable, as is the
over-time pause between powercaps. Explicit temperature calibration fails if a
selected sensor is unavailable. Without thermal control, temperature metrics are
informational and need no calibration file or variance.

Select a declared control aspect explicitly, for example `--IntelCPU.Powercap`,
to apply the limits given by the required `-powercap` option in aggregate or
over-time. Without a selected control, omit `-powercap`: no limits are changed
and the CSV `Powercap` field is `-1`. Explicit `-powercap -1` remains accepted
in that case; positive limits without a selected control fail. At most one powercap control
aspect can be selected. To apply a limit without collecting any metrics, combine
`--no-metrics --IntelCPU.Powercap` with the usual execution arguments.

```sh
./greenmeasurer --aggregate -command "true" -language Shell -program example \
  -output result.csv -n_times 1 -sleep-secs 0 -powercap "-1,5" -time_out_limit 0 \
  --IntelCPU.Powercap --IntelCPU.Package
./greenmeasurer --powercap-calibration -range 5-15 -command "true" \
  -language Shell -program example -output calibration.csv -n_times 1 \
  -sleep-secs 0 -time_out_limit 0 --IntelCPU.Powercap --IntelCPU.Package
```

Powercap calibration requires the control aspect flag, `-range MIN-MAX`, the command, labels, output,
repetition count, sleep, timeout and selected aspect flags. It uses the declared
calibration hook; optional `-compile_cmd` runs before the sweep. If temperature
control is enabled, provide the controlled sensors, calibration files and variance as above.
The supplied Intel hook tests `-1` and every integer in the requested range.
The automatic speedup/greenup report requires a Package energy metric and
exactly one selected `duration` aspect in milliseconds. If several are
selected, the report is skipped; use `Utils/processPowercapCalibrationCSV.py`
with `--time-column` to select the column explicitly.
**Powercap changes hardware limits** and may require privileges.

The Intel implementation stores the saved package/die RAPL limits in
`/var/lib/greenmeasurer/intel-rapl-baseline` on the first powercap operation,
if no saved file exists. It captures the current limits, not necessarily
factory or boot-time settings. `-1` applies that saved baseline when the
powercap control aspect is selected; otherwise, `-1` is only a CSV label.

## Execution and output

Aggregate mode uses no sampling threads. It starts the workload with
`fork()`/`exec()` to retain its PID and enforce timeouts; counters report the
end-minus-start difference and gauges the final reading. Over-time starts one
thread per selected metric aspect in its generated C file; counters are raw readings.
Readers declared with the same header are serialized; readers from different
headers may run concurrently and their libraries must support that.
Samples are aligned to a `Timestamp_ms` grid. `--max-samples` controls the
compile-time capacity (default 1,000,000); overflow fails the measurement.
Unavailable readings are written as `NOT_SUPPORTED`. Aggregate and over-time
append to the CSV, but reject an existing header that does not match the
selected aspects and mode. Powercap calibration may replace its output.
Aggregate CSVs contain `Language,Program,Powercap` and the selected aspect
columns; there is no implicit execution-time column.

With no selected aspects, or if all selected aspects fail setup, the executable
runs the given command once with `system()` and returns its status; it produces
no CSV and does not enforce a timeout or repetitions. If measurement options
were provided, it warns that they will not be used. An empty TOML additionally
omits the measurement runtime from the generated executable:

```sh
python3 scripts/weave.py empty.toml -o generated-plain
./generated-plain/greenmeasurer -command "python3 prog.py"
```

The same command-only invocation also works when aspects are declared but none
are selected.
