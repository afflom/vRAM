# Working with the vRAM product model

The capacity controller imports the product modules from `app/src/Product`. Each facet is a LexLean module. The typed design records describe four views of the device, and the site builder uses those records for its diagrams. Each operator journey names a stakeholder, the features they use and the controller definitions and acceptance theorems for those features. [Product model](../app/src/Product/) · [Controller entry point](../app/src/Contract.lex.tex)

The architecture, design, threat, quality, experience and provenance structures use the schema declarations from Prism-DevRel. The system and release types use the UORust browser-system vocabulary. vRAM extends these with its own evidence, asset and obligation records. The standards named in each obligation identify its scope and supporting files. [Prism facet guide](https://afflom.github.io/prism-devrel/start/explanation/) · [Prism-DevRel source](https://github.com/afflom/prism-devrel/tree/6920ef9d53895b56ef8e68faa16883f80d8d15ff/app/src) · [UORust source](https://github.com/UOR-Foundation/UORust/tree/eca7ec2cc35246d3b507bef8ea2154ad77fffb76)

## Verification and publication

From the repository root, run the pinned SDK checks and export the catalog.

```sh
python3 automation/facet_queries.py
automation/node_modules/.bin/prism-code check .
automation/node_modules/.bin/prism-code build .
automation/node_modules/.bin/prism-code verify .
python3 automation/facets.py > /tmp/vram-facets.json
```

Prism checks the imported modules, proves the controller theorems and replays the application vectors. An offline client can read the complete catalog through the verified Holo's dispatcher. Send the UTF-8 bytes of `F/` to get an ordered JSON array of facet identifiers. Send `F/architecture`, or another identifier prefixed by `F/`, to get that group's title, summary, evidence and records as UTF-8 JSON. An unknown request returns the bytes of `invalid request`. The browser's record inspector uses this same interface. The pinned SDK's internal architecture, quality and security summary arrays remain empty. [Live record inspector](https://afflom.github.io/vRAM/runtime/#facet-heading)

When a declaration, registered claim or cited source line changes, regenerate the derived query module with `python3 automation/facet_queries.py --write`. Run the same command without the flag to check for drift. The release compares each response from the built Holo byte for byte with the independently exported group. Lean proves that a found payload passes through unchanged and that a missing lookup is refused. [Reply policy](../app/src/FacetPolicy.lex.tex) · [Archive verification](../automation/verify-facet-queries.mjs)

The catalog check resolves model symbols, evidence files, numeric references, feature journeys, state-machine entry and transitions, page-read message direction and threat mappings. It recomputes the brand source-span identities from the cited lines and verifies the text or token against the brand guide. The release model identifies the application with SHA-256 of the UTF-8 `Contract::application` declaration, including the closing brace. Adapter identities use SHA-256 of the browser SDK package lock, Linux package assembler or exact `actions/deploy-pages@<commit>` reference. The independent SDK verification reports the Holo's content identity. [Facet exporter](../automation/facets.py) · [System and release model](../app/src/Product/System.lex.tex)

For a negative check in an isolated copy, changing the destination of `Product.Architecture::edgeLaunch` from component 2 to 999 made `python3 automation/facets.py` exit 1 with `Product.Architecture::edgeLaunch has dangling toIndex`. Restoring component 2 produced 219 rows in ten facets. The same model passed `prism-code verify .` with 27 modules, 49 proved theorems, 68 passing acceptance vectors and verified identities at assurance level P0.

A second negative check changed one byte of the archived workstation launcher name. The source check reported `Stale facet queries`, and replaying that Holo against the catalog failed with `Facet reply differs from the catalog for F/architecture`. Restoring the generated module verified all 222 records, including the architecture, security and quality collections.
