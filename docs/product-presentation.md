# Publishing the vRAM product

The Linux package, browser controller and product site are the three release targets declared in Product.System. The site links the installable package to the same device workflow that exercised it. The homepage explains where each Linux kernel reports the added capacity. Its performance figures identify the workload, units and historical qualification receipt. [System declarations](../app/src/Product/System.lex.tex) · [Linux usage](../host/packaging/USAGE.md)

## Facets and diagrams

The facet reference groups the product declarations under architecture, design, features, security, quality, behavior, compliance, system, extension and application. Each record links to its declaration. The behavior section also exposes the registered claims with their proof or evidence basis. The controller serves all ten groups directly from its Holo. Publication verifies each archived reply against the source catalog, checks the published source snapshot, and binds the catalog digest and archive identity in `app.json`. Standards references describe the vocabulary and evidence used by the model. [Prism facet guide](https://afflom.github.io/prism-devrel/start/explanation/) · [Catalog source](../app/src/Product/Catalog.lex.tex) · [Model validation and upstream schema versions](model-facets.md)

The backing-reservation view, memory device topology, page-read sequence and workstation admission diagram all use the same typed design records as the reference tables. Each figure has a text rendering of the relationships and a scalable SVG download. The brand guide documents the visual assets, theme tokens, font licenses and messaging. [Design declarations](../app/src/Product/Design.lex.tex) · [Brand guide](brand.md)

## Reproduce the public bundle

On the package build host, install Graphviz and the dependencies in the device workflow. Use the exact Debian package produced by that workflow and an empty output directory. The publisher invokes the locked Prism tool to build and verify the archive, checks local links and packages the assets and checksums. Serve the directory over HTTP to run the browser checks. [Device workflow](../.github/workflows/device.yml)

```sh
npm ci --prefix automation --ignore-scripts
PRISM_CODE_TIER=offline PRISM_LEXLEAN_FETCH=0 PRISM_TELEMETRY=0 \
  python3 automation/product_site.py --package dist/vram-host_0.6.6_amd64.deb --out product-public
python3 -m http.server 8765 --directory product-public --bind 127.0.0.1
```

In a second terminal, run the following command with Chrome installed. Set `CHROME_BIN=chromium` for Chromium.

```sh
node automation/test-product-site.mjs http://127.0.0.1:8765/
```

The browser gate exercises home, reference and controller pages at phone width, checks theme persistence between pages, evaluates changed inputs and runs the acceptance suite. Replacing the generated controller's decoded decision with a constant `advance` made this command fail with `Timed out waiting for ceiling decision`. Restoring the controller produced the three expected decisions and passed all 68 vectors. The device workflow publishes the checked directory to GitHub Pages after the Linux package tests pass.
