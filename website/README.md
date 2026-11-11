# GreenMeasurer website

The website is a dependency-free static site:

- `index.html`: overview and links to the documentation.
- `guide.html`: installation, configuration, modes and powercap warnings.
- `fibonacci.html`: a GreenMeasurer usage walkthrough with a Python sample workload and standard/over-time CSVs.
- `metrics.html`: metric names, reading functions and sources.
- `reference.html`: backend callbacks, metric definitions and developer tools.
- `styles.css`, `favicon.svg` and `logo.svg`: shared presentation and assets.

The documentation describes the current Linux implementation. macOS and
Windows are not currently supported; adding the shared interfaces alone does
not provide the OS-specific measurements, build support or validation needed
for those platforms.

## Preview

Open `index.html` directly or run:

```sh
python3 -m http.server 8000 --directory website
```

Visit `http://localhost:8000`. No JavaScript, build step or package manager is
required. Copy the complete directory to a static host or GitHub Pages.

## Maintenance

Update the guide and catalogue when the TOML schema, modes, metrics or
hardware behavior change. Update the backend API reference when callback
contracts, extension mechanisms or build behavior change. Check the actual
source and hardware validation status before documenting powercap behavior;
`-1` leaves existing limits unchanged.
Keep the authors on the overview and the submission and acknowledgements in
each page footer consistent when the paper details change.
