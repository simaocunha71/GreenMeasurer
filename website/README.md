# GreenMeasurer documentation website

The website documents the TOML-only interface. See the
[main README](../README.md) for full generation, execution and calibration
instructions.

Standalone, dependency-free static website. The page is built from `index.html`,
`styles.css` and `favicon.svg`; it does not need JavaScript, a bundler or a package
manager.

## Preview locally

Open `index.html` in a browser, or serve this directory over HTTP:

```sh
python3 -m http.server 8000 --directory website
```

Then visit `http://localhost:8000`.

## Publish from another repository

1. Copy the contents of this `website/` directory to the root of the hosting
   repository (or configure its static-site host to use this directory).
2. Enable GitHub Pages for the branch and folder containing `index.html`, or
   deploy the directory to any static web host.
3. Keep the files together so the relative stylesheet and favicon paths
   continue to work.

No build step, framework runtime, API server or package installation is required.

## Documentation maintenance

The backend and metric tables, TOML examples and diagnostic guidance describe
the current GreenMeasurer source repository. Update them when backend
descriptors, sensor APIs, sampling semantics, CLI options or build dependencies
change. Metrics are declared by selected backends and may vary between machines.
Default startup output hides undetected, unbuilt and lower-priority backends;
`GREENMEASURER_VERBOSE_BACKENDS=1` shows the full diagnostic list.

Both modes support explicit selection and `select = "all"`, which includes all
system metrics, metrics of the CPU/GPU backends selected at runtime, and metrics
in all declared external adapters and source definitions. Example commands overwrite
their configured CSV files, and external adapters execute native code.
The C generator embeds
the existing engine rather than producing specialized metric code, and
also builds trusted numeric functions declared in `[[external_metrics]]` from
C sources; see `examples/configurations/external-source.toml`. External reads
use a separate shared library per metric and can be selected by short name or
function name. Built-in metrics remain in their backend's shared library.
Missing or incompatible adapters are skipped with diagnostics. Reads
that fail after preflight are recorded as `NOT_SUPPORTED`. Backend metrics
failing preflight are skipped, and later backend failures are recorded as
`NOT_SUPPORTED`. Configured system readings follow the same preflight/failed
sample policy where a preflight read is possible. Included backends also expose a per-metric
function map that permits selection by short name or function name.
Backend descriptions, origins, functions and units can be inspected with
`./src/main --list-metrics`. Built-in backends define these from a single
metric registry; new CPU/GPU backend templates live in `examples/backend_template/`.
The general getting-started guide explains how to measure any foreground command;
the separate step-by-step C Fibonacci walkthrough uses the standard, overtime,
temperature-calibration and powercap-calibration examples in
`examples/configurations/`. Keep the displayed TOML in sync with those files
and label sample results as illustrative; temperature calibration produces a
text file, not a CSV.
