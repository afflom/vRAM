# Working with the vRAM product model

The capacity controller imports the product modules from `app/src/Product`. Each facet is a LexLean module. The typed design records describe four views of the device, and the site builder uses those records for its diagrams. Each operator journey names a stakeholder, the features they use and the controller definitions and acceptance theorems for those features. [Product model](../app/src/Product/) · [Controller entry point](../app/src/Contract.lex.tex)

The architecture, design, threat, quality, experience and provenance structures use the schema declarations from Prism-DevRel. The system and release types use the UORust browser-system vocabulary. vRAM extends these with its own evidence, asset and obligation records. The standards named in each obligation identify its scope and supporting files. [Prism facet guide](https://afflom.github.io/prism-devrel/start/explanation/) · [Prism-DevRel source](https://github.com/afflom/prism-devrel/tree/6920ef9d53895b56ef8e68faa16883f80d8d15ff/app/src) · [UORust source](https://github.com/UOR-Foundation/UORust/tree/eca7ec2cc35246d3b507bef8ea2154ad77fffb76)

## Verification and publication

From the repository root, run the pinned SDK checks and export the catalog.

```sh
automation/node_modules/.bin/prism-code check .
automation/node_modules/.bin/prism-code build .
automation/node_modules/.bin/prism-code verify .
python3 automation/facets.py > /tmp/vram-facets.json
```

Prism checks the imported modules, proves the controller theorems and replays the application vectors. The current prism-code 0.11 evaluator emits empty architecture, quality and security arrays in the executable Holo's model document. The product site publishes the typed records from the checked source tree alongside the verified executable. Its JSON entries carry qualified declaration names, types, fields and source locations. The application summary counts its vectors, and the behavior facet includes claims with their registered assurance levels. [Publication code](../automation/product_site.py) · [Claim register](../model/ids.toml)

The catalog check resolves model symbols, evidence files, numeric references, feature journeys, state-machine entry and transitions, page-read message direction and threat mappings. It recomputes the brand source-span identities from the cited lines and verifies the text or token against the brand guide. The release model identifies the application with SHA-256 of the UTF-8 `Contract::application` declaration, including the closing brace. Adapter identities use SHA-256 of the browser SDK package lock, Linux package assembler or exact `actions/deploy-pages@<commit>` reference. The independent SDK verification reports the Holo's content identity. [Facet exporter](../automation/facets.py) · [System and release model](../app/src/Product/System.lex.tex)

For a negative check in an isolated copy, changing the destination of `Product.Architecture::edgeLaunch` from component 2 to 999 made `python3 automation/facets.py` exit 1 with `Product.Architecture::edgeLaunch has dangling toIndex`. Restoring component 2 produced 219 rows in ten facets. The same model passed `prism-code verify .` with 27 modules, 49 proved theorems, 68 passing acceptance vectors and verified identities at assurance level P0.
