# GreenMeasurer website

The dependency-free static site documents the current configuration-driven,
aspect-oriented weaver:

- `index.html`: overview, workflow, authors and acknowledgements.
- `guide.html`: setup, declarations, measurement modes and calibration.
- `fibonacci.html`: the four-stage Fibonacci sequence.
- `metrics.html`: explicitly declared metrics and optional device readers.
- `reference.html`: generated code, TOML field reference, device hooks and tools.
- `styles.css`, `favicon.svg` and `logo.svg`: shared presentation and assets.
- `website.zip`: a packaged copy of the site.

The old `architecture.drawio.png` and `diagram.drawio.png` are retained as
historical assets; they depict the removed engine and are not used by the
current pages.

## Preview

Open `index.html` directly or run:

```sh
python3 -m http.server 8000 --directory website
```

Visit `http://localhost:8000`. No JavaScript, build step or package manager is
required.

## Maintenance

Check the source, [project README](../README.md) and
[weaver documentation](../docs/WEAVER.md) when updating the site. Keep examples,
hardware warnings, authors and funding acknowledgements consistent with the
project. If the site changes, refresh the corresponding files in `website.zip`;
the historical diagrams remain in the archive but are not used by the pages.
