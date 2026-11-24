"""Generate the pure vRAM facet query from the validated product declarations."""
import argparse
import json

from facets import SRC, catalog


def literal(data):
    return "0x" + data.encode("utf-8").hex()


def encode(value):
    return literal(json.dumps(value, separators=(",", ":"), ensure_ascii=False))


def query_module(groups):
    replies = {"F/": encode([g["id"] for g in groups])}
    for group in groups:
        replies["F/" + group["id"]] = encode(group)
    rows = [r"\begin{lexlean}{Product.FacetQueries}",
            r"\useglossary{lexlean.std.bool@1.2.0}",
            r"\title{Boolean}", r"\begin{semanticmodule}",
            "definition{", "  name: lookup, parameters: [request : Bytes], result: (Option Bytes),"]
    for i, (request, reply) in enumerate(replies.items()):
        prefix = "  body: " if i == 0 else "    "
        rows.append(f"{prefix}(if (prim equal request {literal(request)} : Bool) then (ctor Option.some [Bytes] {reply}) else")
    rows += ["    " + "(ctor Option.none [Bytes])" + ")" * len(replies) + ",",
             "  axioms: [Quot.sound, propext]", "}", r"\end{semanticmodule}", r"\end{lexlean}"]
    return "\n".join(rows) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="refresh the derived LexLean source")
    args = parser.parse_args()
    source = SRC / "Product/FacetQueries.lex.tex"
    expected = query_module(catalog()["facets"])
    if args.write:
        source.write_text(expected)
    elif not source.is_file() or source.read_text() != expected:
        parser.exit(1, "Stale facet queries. Run python3 automation/facet_queries.py --write\n")
