# Fibonacci configuration examples

These examples show how to declare existing device functions as metrics in TOML.
Each folder contains the four modes below. The measurement modes run Python
Fibonacci with argument `30` once:

- `standard.toml`: one CSV row per run; counters are differences between
  readings and gauges are the final readings.
- `overtime.toml`: periodically samples each selected aspect and writes
  aligned CSV rows. Counters are raw readings.
- `powercap-calibration.toml`: measures the saved powercap baseline and a
  short range of CPU power limits.
- `temperature-calibration.toml`: waits one second and records a temperature
  baseline. It does not run Fibonacci or measure the other metrics.

Choose the configuration set that matches your experiment:

- `intel/`: Intel CPU and Intel GPU readers.
- `nvidia/`: NVIDIA GPU readers.
- `system/`: system readers.
- The TOMLs directly in `examples/`: all four devices together, including both
  GPUs in the same run.

The Intel CPU and Intel GPU are separate aspects because each declares its own
header, library and setup, and over-time mode uses a sampling thread per
selected aspect. In `nvidia/` and `system/`, powercap calibration adds an
Intel CPU aspect for its thermal and powercap hooks and Package energy. Their
temperature-calibration TOMLs use only the Intel CPU: this mode records a CPU
baseline and does not read the NVIDIA or system metrics.

## Requirements and running

From the repository root, build the available device libraries and generate an
executable:

```sh
make -C src
python3 scripts/weave.py examples/system/standard.toml -o generated-example-system
./generated-example-system/greenmeasurer
```

Replace the TOML path and output directory with the desired example.
Generation does not run the workload. Run the executable from the repository
root, with the permissions required to access the devices. TOML header and
library paths are relative to the TOML; workload and output paths are relative
to the directory from which the executable is run.

To build and run all example TOMLs with a per-configuration summary, use
`bash scripts/test_examples.sh`. Results and logs are kept in separate
directories. See [the scripts guide](../scripts/README.md#check-all-examples)
for status meanings and powercap precautions.

All declared libraries must be built and linkable. Intel CPU configurations
require the CPU library's dependencies; NVIDIA configurations require NVML
development and runtime libraries. Standard and over-time examples in
`system/` need neither Intel nor NVIDIA libraries. This remains true for
aspects marked `optional = true`: that setting allows a run to continue after
an aspect's setup fails, but does not remove build dependencies. On machines
without a declared library, remove its entire `[[aspect]]` block and its
`[[aspect.metric]]` blocks before generating.

The CPU example requires a supported Intel CPU. Readings also depend on
hardware, drivers and permissions. Failed readings and metrics of optional
aspects that fail setup appear as `NOT_SUPPORTED` in the CSV. The Intel GPU
temperature reader currently selects the first hwmon sensor labelled `GPU`;
verify its source on systems with multiple GPUs. NVIDIA VRAM uses the workload
process tree's root PID. Workload-scoped gauges can be unavailable after a
short program exits, especially in standard mode.

**Powercap calibration changes CPU limits.** The range `5..5` W is only an
example, not a recommendation: set limits appropriate for the machine before
running, and ensure the required permissions and dependencies are available.
Each powercap example uses the baseline file produced by the temperature
example in the same folder; run that temperature example first for the
configured one-second wait, or the runtime will create a missing baseline
immediately. One second is for demonstration, not a validated stabilization
period. Calibration overwrites the baseline file, and powercap calibration may
replace its output. Standard and over-time append to their CSVs; use fresh
output files when changing metrics.

## Adapting a configuration

Change `command`, `language`, `program`, `output` and the mode settings for
your experiment. Keep only the aspect and metric declarations you need, or
add your own compatible reader functions. Each metric has a name, a
no-argument `read` signature, a `counter` or `gauge` kind, and an optional
unit. Generate a new executable after editing the TOML.

See [the weaver guide](../docs/WEAVER.md) for all settings and aspect hooks.
