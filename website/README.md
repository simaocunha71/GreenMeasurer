# GreenMeasurer documentation website

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

The backend and metric tables describe the implementation in the GreenMeasurer
source repository. Update them when backend descriptors, sensor APIs, sampling
semantics, CLI options or build dependencies change. In particular, metrics are
declared by the selected backend and may vary between machines.
