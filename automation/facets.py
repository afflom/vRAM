"""Publish the literal facet declarations in the same source tree that Prism verifies."""
import hashlib
import json
from pathlib import Path
import re
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "app/src"
MODULES = {
    "architecture": "Architecture", "design": "Design", "features": "Features",
    "security": "Security", "quality": "Quality", "behavior": "Behavior",
    "compliance": "Compliance", "system": "System", "extension": "Brand",
    "application": "Application",
}
DECLARATION = re.compile(r"^(?:definition|theorem)\{\s*name:\s*(\w+)", re.M)
LITERAL = re.compile(
    r"^definition\{\s*name:\s*(\w+),\s*parameters:\s*\[\],\s*result:\s*([\w.:]+),"
    r"\s*body:\s*(\(record\s+.*?\))\s*^\}", re.M | re.S)
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|:=|[(){},]|[^\s(){},]+')
REFERENCE = re.compile(r"[A-Z][\w.]*::\w+")


def require(condition, message):
    if not condition:
        raise ValueError(message)


class Literal:
    """Read presentation data only. Prism checks types, behavior, proofs and vectors."""

    def __init__(self, expression):
        self.tokens = iter(TOKEN.findall(expression))

    def expect(self, expected):
        require(next(self.tokens) == expected, f"expected {expected}")

    def read(self):
        token = next(self.tokens)
        if token.startswith('"'):
            return json.loads(token)
        if token.isdecimal():
            return int(token)
        if token in ("true", "false"):
            return token == "true"
        if token.startswith("0x"):
            return token
        require(token == "(", f"unsupported literal {token}")
        operation = next(self.tokens)
        if operation == "record":
            next(self.tokens)  # The type is checked by LexLean.
            self.expect("{")
            value = {}
            name = next(self.tokens)
            while name != "}":
                self.expect(":=")
                require(name not in value, f"duplicate field {name}")
                value[name] = self.read()
                name = next(self.tokens)
                if name == ",":
                    name = next(self.tokens)
        elif operation.isdecimal():
            self.expect(":")
            require(next(self.tokens) in ("UInt32", "UInt8"), "unsupported integer literal type")
            value = int(operation)
        elif operation == "ctor":
            value = next(self.tokens).rsplit(".", 1)[-1]
        elif operation == "nil":
            next(self.tokens)
            value = []
        elif operation == "cons":
            head = self.read()
            value = [head, *self.read()]
        else:
            raise ValueError(f"unsupported literal operation {operation}")
        self.expect(")")
        return value


def declarations(path):
    content = path.read_text()
    module = re.match(r"\\begin\{lexlean\}\{([\w.]+)\}", content)[1]
    result = []
    for match in LITERAL.finditer(content):
        name, kind, expression = match.groups()
        parser = Literal(expression)
        fields = parser.read()
        require(next(parser.tokens, None) is None, f"trailing tokens in {module}::{name}")
        result.append(dict(id=f"{module}::{name}", kind=kind, fields=fields,
                           source=f"{path.relative_to(ROOT)}#L{1 + content.count(chr(10), 0, match.start())}"))
    return result


def evidence_path(value):
    path = (ROOT / value).resolve()
    require(path.is_relative_to(ROOT) and path.is_file(), f"missing repository evidence {value}")


def check_fields(value, field, symbols):
    if isinstance(value, dict):
        for key, child in value.items():
            check_fields(child, key, symbols)
    elif isinstance(value, list):
        for child in value:
            check_fields(child, field, symbols)
    elif isinstance(value, str):
        if REFERENCE.fullmatch(value):
            require(value in symbols, f"unresolved model reference {value}")
        elif field in ("path", "evidence", "reference", "location"):
            evidence_path(value)


def check_records(records, symbols):
    indexed = {}
    numeric_refs = {
        "fromIndex": "Component", "toIndex": "Component", "componentIndex": "Component",
        "stakeholderIndex": "Stakeholder", "concernIndex": "Concern", "viewpointIndex": "Viewpoint",
        "assetIndex": "Asset", "threatIndex": "Threat", "riskIndex": "Risk",
        "characteristicIndex": "ProductQualityCharacteristic", "subcharacteristicIndex": "QualitySubcharacteristic",
        "requirementIndex": "QualityRequirement",
    }
    for row in records:
        fields = row["fields"]
        check_fields(fields, "", symbols)
        if "index" in fields:
            kind = row["kind"].split("::")[-1]
            ids = indexed.setdefault(kind, set())
            require(fields["index"] not in ids, f"duplicate {kind} index {fields['index']}")
            ids.add(fields["index"])
    for row in records:
        for field, target in numeric_refs.items():
            if field in row["fields"]:
                require(row["fields"][field] in indexed[target], f"{row['id']} has dangling {field}")
    for kind, ids in indexed.items():
        require(ids == set(range(len(ids))), f"nonconsecutive {kind} indices")


def check_relationships(records):
    by_kind = {}
    for row in records:
        by_kind.setdefault(row["kind"].split("::")[-1], {})[row["id"]] = row["fields"]
    components = {ref: fields["index"] for ref, fields in by_kind["Component"].items()}
    edges = {(row["fromIndex"], row["toIndex"]) for row in by_kind["Edge"].values()}
    properties = {**by_kind["SystemProcess"], **by_kind["SystemState"]}
    require(all(row["owner"] in components for row in properties.values()), "invalid process or state owner")
    for msg in by_kind["UmlMessage"].values():
        lines = {r["component"] for r in by_kind["UmlLifeline"].values() if r["view"] == msg["view"]}
        require(msg["source"] in lines and msg["target"] in lines and
                (components[msg["source"]], components[msg["target"]]) in edges and
                by_kind["SystemProcess"][msg["process"]]["owner"] == msg["target"],
                "sequence message lacks its directed component edge or recipient process")
    for name, view in by_kind["DesignView"].items():
        if view["diagram"] == "umlStateMachine":
            states = [r for r in by_kind["UmlState"].values() if r["view"] == name]
            require(sum(row["initial"] for row in states) == 1, f"{name} requires one initial state")
            membership = {r["state"] for r in states}
            for t in by_kind["UmlTransition"].values():
                if t["view"] == name:
                    require(t["source"] in membership and t["target"] in membership,
                            f"transition leaves {name}")
    features = by_kind["Feature"]
    covered = set()
    for j in by_kind["Journey"].values():
        require(j["actor"] in by_kind["Stakeholder"] and bool(j["steps"]), "invalid journey")
        for ref in j["steps"]:
            require(ref in features, f"journey step {ref} is not a feature")
            covered.add(ref)
    require(covered == set(features), "feature has no user journey")
    mappings = {r["threat"]: r["controls"] for r in by_kind["ThreatMapping"].values()}
    require(set(mappings) == set(by_kind["Threat"]), "threat lacks a control mapping")
    for threat, controls in mappings.items():
        require(controls and all(c in by_kind["ApplicationSecurityControl"] for c in controls),
                f"missing control for {threat}")

    citations = {}
    for source in by_kind["SourceEntity"].values():
        location = source["location"]
        lines = (ROOT / location).read_text().splitlines()
        start, end = source["firstLine"], source["lastLine"]
        require(1 <= start <= end <= len(lines), f"invalid source span in {location}")
        excerpt = "\n".join(lines[start-1:end])
        digest = hashlib.sha256()
        for part in ("prism/source-span/1", location, excerpt):
            data = part.encode()
            digest.update(len(data).to_bytes(8, "big") + data)
        require(digest.hexdigest() == source["kappa"], f"stale source span {location}:{start}")
        citations[source["index"]] = excerpt
    for row in (*by_kind["BrandElement"].values(), *by_kind["DesignToken"].values()):
        require(row.get("value", row.get("text")) in citations[row["source"]], "brand text differs from its source")


def check_release(records, application):
    system = next(r["fields"] for r in records if r["kind"].endswith("::SystemModel"))
    identity = "sha256:" + hashlib.sha256(application.encode()).hexdigest()
    require(system["applicationProfile"]["applicationModelDigest"] == identity, "stale application model digest")
    adapter_sources = {"linux": "host/package.py", "linux-kernel": "host/kernel/package.sh", "pages": ".github/workflows/device.yml",
                       "browser": "automation/package-lock.json"}
    for target in system["targets"]:
        path = ROOT / adapter_sources[target["id"]]
        material = path.read_bytes()
        if target["id"] == "pages":
            material = re.search(rb"uses: (actions/deploy-pages@[a-f0-9]{40})", material)[1]
        digest = hashlib.sha256(material).hexdigest()
        require(target["adapterDigest"] == "sha256:" + digest, f"stale adapter digest for {path.name}")
    components = {r["id"]: r["artifact"] for r in system["components"]}
    targets = {r["id"] for r in system["targets"]}
    artifacts = {r["id"] for r in system["artifacts"]}
    manifests = [r["fields"] for r in records if r["kind"].endswith("::SystemManifest")]
    require({r["targetId"] for r in manifests} == targets, "release targets lack manifests")
    require({r["artifactId"] for r in manifests} == artifacts, "release artifacts lack manifests")
    for manifest in manifests:
        require(manifest["productId"] == system["product"]["id"] and
                manifest["productVersion"] == system["product"]["version"] and
                manifest["applicationModelDigest"] == identity and
                manifest["artifactId"] == components[manifest["componentId"]] and
                manifest["artifactId"] in artifacts and manifest["controls"] == system["controls"],
                f"unresolved system manifest {manifest['targetId']}")


def catalog():
    product = SRC / "Product"
    metadata = declarations(product / "Catalog.lex.tex")
    facets = []
    symbols = set()
    for path in SRC.rglob("*.lex.tex"):
        module = str(path.relative_to(SRC)).removesuffix(".lex.tex").replace("/", ".")
        symbols.update(f"{module}::{name}" for name in DECLARATION.findall(path.read_text()))
    for meta in metadata:
        data = meta["fields"]
        slug = data["slug"]
        path = product / (MODULES[slug] + ".lex.tex")
        rows = declarations(path)
        require(len(rows) == len(DECLARATION.findall(path.read_text())),
                f"unrendered declarations in {path}")
        require(bool(rows), f"empty facet {slug}")
        check_fields(data, "", symbols)
        facets.append(dict(id=slug, title=data["title"], summary=data["summary"],
                           evidence=data["evidence"], records=rows))
    by_id = {f["id"]: f for f in facets}
    require(len(facets) == len(MODULES) and by_id.keys() == MODULES.keys(), "incomplete facet catalog")
    records = [r for f in facets for r in f["records"]]
    check_records(records, symbols)
    check_relationships(records)
    contract = SRC / "Contract.lex.tex"
    application_text = re.search(r"^definition\{\s*name: application,.*?^\}", contract.read_text(), re.M | re.S)[0]
    check_release(by_id["system"]["records"], application_text)
    app = next(r for r in declarations(contract) if r["id"] == "Contract::application")
    app["fields"]["acceptanceVectors"] = len(app["fields"]["acceptanceVectors"])
    by_id["application"]["records"].append(app)
    register = tomllib.loads((ROOT / "model/ids.toml").read_text())
    for claim in register["id"]:
        by_id["behavior"]["records"].append(dict(id=claim["id"], kind="RegisteredClaim",
            fields={k: v for k, v in claim.items() if k != "id"}, source="model/ids.toml"))
    return dict(schema="vram/product-facets/1", facets=facets)


if __name__ == "__main__":
    try:
        print(json.dumps(catalog(), ensure_ascii=False, indent=2))
    except (ValueError, KeyError, StopIteration) as error:
        sys.exit(f"facet catalog: {error}")
