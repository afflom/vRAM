"""Assemble the product site from a verified Prism build and the tested Debian package."""
import argparse
import hashlib
from design_views import design_views
from html import escape
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from urllib.parse import unquote, urlsplit
from zipfile import ZipFile, ZIP_DEFLATED

root = Path(__file__).resolve().parents[1]
repo = "https://github.com/afflom/vRAM"
base = "https://afflom.github.io/vRAM/"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--package", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True, help="new or empty build directory")
args = parser.parse_args()
out = args.out.resolve()
if out.exists() and any(out.iterdir()):
    parser.error("output directory must be empty")
package = args.package.resolve(strict=True)
version = subprocess.check_output(["dpkg-deb", "-f", package, "Version"], text=True).strip()
if package.name != f"vram-host_{version}_amd64.deb":
    parser.error("expected the versioned amd64 package")
prism = root / "automation/node_modules/.bin/prism-code"


def checked(command):
    proc = subprocess.run([prism, command, ".", "--json"], cwd=root, stdout=subprocess.PIPE, text=True)
    result = json.loads(proc.stdout)
    if not result["ok"]:
        raise ValueError(f"Prism {command} failed: {result['diagnostics']}")
    return result["summary"]


def write_json(name, value):
    (out / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


subprocess.run([sys.executable, root / "automation/facet_queries.py"], check=True)
built = checked("build")
verified = checked("verify")
if not verified["identitiesVerified"] or built["address"] != verified["address"]:
    raise ValueError("unverified model release")
shutil.copytree(root / "site", out, dirs_exist_ok=True)
shutil.copytree(root / "app/.prism/site", out / "runtime", dirs_exist_ok=True)
record = json.loads((out / "runtime/app.holo.json").read_text())
holo = out / "runtime/app.holo"
if record["holo"] != verified["address"] or holo.read_bytes() != (root / verified["artifact"]).read_bytes():
    raise ValueError("published Holo differs from the verified model")
sdk = root / "automation/node_modules/prism-code"
shutil.copytree(sdk / "src", out / "runtime/vendor/src")
shutil.copy2(sdk / "LICENSE", out / "runtime/vendor/LICENSE")
downloads = out / "downloads"
downloads.mkdir()
shutil.copy2(package, downloads / package.name)
digest = hashlib.sha256(package.read_bytes()).hexdigest()
dkms = (root / "host/kernel/dkms.conf").read_text()
kernel_version, = re.findall(r'^PACKAGE_VERSION="([^"]+)"$', dkms, re.M)
kernel_name = f"vram-kernel-dkms_{kernel_version}_amd64.deb"
kernel_package = package.parent / kernel_name
if kernel_package.is_file():
    shutil.copy2(kernel_package, downloads / kernel_name)
else:
    subprocess.run(["bash", root / "host/kernel/package.sh", downloads / kernel_name], check=True)
if subprocess.check_output(["dpkg-deb", "-f", downloads / kernel_name, "Version"], text=True).strip() != kernel_version:
    raise ValueError("kernel package version differs from DKMS source")
kernel_digest = hashlib.sha256((downloads / kernel_name).read_bytes()).hexdigest()
(downloads / "SHA256SUMS").write_text(f"{digest}  {package.name}\n{kernel_digest}  {kernel_name}\n")
with ZipFile(out / "assets/brand-kit.zip", "w", ZIP_DEFLATED) as archive:
    for f in sorted((out / "assets").rglob("*")):
        if f.is_file() and f.suffix != ".zip":
            archive.write(f, Path("site") / f.relative_to(out))
    guide = (root / "docs/brand.md").read_text()
    guide = guide.replace("(../README.md", f"({repo}/blob/main/README.md")
    guide = guide.replace("(device-verification.md", f"({repo}/blob/main/docs/device-verification.md")
    archive.writestr("docs/brand.md", guide)
    for name in ("site/styles.css", "LICENSE", "LICENSE-MIT", "LICENSE-APACHE"):
        archive.write(root / name, name)
    for f in sorted((root / "docs/brand").rglob("*")):
        if f.is_file():
            archive.write(f, f.relative_to(root))
facets = json.loads(subprocess.check_output([sys.executable, root / "automation/facets.py"], cwd=root))
for src in (root / "app/src").rglob("*.lex.tex"):
    snapshot = out / "runtime/model/src" / src.relative_to(root / "app/src")
    if src.read_bytes() != snapshot.read_bytes():
        raise ValueError(f"release source differs from {src}")
facets["semanticId"] = record["semanticId"]
write_json("facets.json", facets)
subprocess.run(["node", root / "automation/verify-facet-queries.mjs", holo, out / "facets.json"], check=True)
release = next(r["fields"] for f in facets["facets"] for r in f["records"] if r["kind"].endswith("::SystemModel"))
if release["product"]["version"] != version:
    raise ValueError("package and modeled release versions differ")
write_json("app.json", dict(name=record["name"], shortName="vRAM", live=base+"runtime/",
    purpose="Evaluate capacity checkpoints and replay the verified vRAM model in a browser.",
    repository=repo, report=repo+"/blob/main/CONFORMANCE.md", source="runtime/model/project.json",
    archive="runtime/app.holo", identity=record, facets="facets.json", facetIndexRequest="F/",
    facetsSha256=hashlib.sha256((out / "facets.json").read_bytes()).hexdigest(),
    package=dict(version=version, path="downloads/"+package.name, sha256=digest)))
landing = (out / "index.html").read_text()
header = re.search(r'<header\b.*?</header>', landing, re.S)[0]
footer = re.search(r'<footer\b.*?</footer>', landing, re.S)[0]
head = re.search(r'<head>.*?</head>', landing, re.S)[0]


def chrome(title, description, path):
    html = re.sub(r'<title>.*?</title>', f"<title>{title} · vRAM</title>", head)
    html = re.sub(r'(name="description" content=")[^"]+', lambda m:m[1]+escape(description, quote=True), html)
    html = re.sub(r'((?:property="og:title"|name="twitter:title") content=")[^"]+', lambda m:m[1]+title+" · vRAM", html)
    html = re.sub(r'((?:property="og:description"|name="twitter:description") content=")[^"]+', lambda m:m[1]+escape(description, quote=True), html)
    html = html.replace(f'"{base}"', f'"{base}{path}"')
    return html.replace("</head>", '<link rel="stylesheet" href="reference.css"></head>')


def display(value):
    if isinstance(value, list):
        return ", ".join(display(item) for item in value)
    if isinstance(value, dict):
        return ", ".join(f"{escape(key)} = {display(item)}" for key, item in value.items())
    value = str(value)
    if value in record_ids:
        return f'<a href="#{escape(value)}">{escape(value)}</a>'
    return escape(value)


def facet_section(f):
    rows = []
    for r in f["records"]:
        fields = "".join(f"<dt>{escape(k)}</dt><dd>{display(v)}</dd>" for k, v in r["fields"].items())
        rows.append(f'<li id="{escape(r["id"])}"><code>{escape(r["kind"])} · {escape(r["id"])}</code>'
                    f'<dl>{fields}</dl><a href="{repo}/blob/main/{escape(r["source"])}">Source declaration</a></li>')
    evidence = []
    for path in f["evidence"]:
        target = path if path.startswith("https://") else f"{repo}/blob/main/{path}"
        evidence.append(f'<a href="{escape(target, quote=True)}">{escape(path)}</a>')
    return (f'<section id="{escape(f["id"])}" class="ref-group"><h2>{escape(f["title"])}</h2>'
            f'<p>{escape(f["summary"])}</p><p class="ref-links">{" ".join(evidence)}</p>' +
            (views if f["id"] == "design" else "") +
            f'<details><summary>Explore {len(rows)} model records</summary><ul class="record-list">{"".join(rows)}</ul></details></section>')


groups = facets["facets"]
record_ids = {r["id"] for f in groups for r in f["records"]}
views = design_views(groups, out / "diagrams")
nav = "".join(f'<a href="#{escape(f["id"])}">{escape(f["title"])}</a>' for f in groups)
intro = "Explore the architecture, design decisions, Linux workflows and evidence behind vRAM."
facet_html = (f'<!doctype html><html lang="en">{chrome("Model facets", intro, "facets.html")}<body class="reference">'
              '<a class="skip" href="#main">Skip to content</a>'+header.replace('href="facets.html"', 'href="facets.html" aria-current="page"')+
              '<main id="main" class="ref-main"><p class="ref-crumb"><a href="./">vRAM</a> / Reference</p>'
              f'<h1>Model facets</h1><p class="ref-intro">{intro}</p><p class="ref-links"><a href="facets.json">Download facet records</a>'
              '<a href="runtime/">Run the capacity controller</a><a href="https://afflom.github.io/prism-devrel/start/explanation/">Prism facet guide</a></p><div class="ref-layout">'
              f'<aside class="ref-toc"><nav aria-label="Facets">{nav}</nav></aside><div>'+
              "".join(facet_section(f) for f in groups)+'</div></div></main>'+footer+'</body></html>')
(out / "facets.html").write_text(facet_html)
runtime = out / "runtime/index.html"
template = out / "runtime/view.html"
def parent_links(markup):
    return re.sub(r'((?:href|src)=")(?!https?:|#)([^"]+)', r'\1../\2', markup)


runtime_head = chrome("Capacity controller", "Evaluate a trial with the verified capacity model.", "runtime/")
runtime.write_text('<!doctype html><html lang="en">' + parent_links(runtime_head) +
                   '<body class="reference"><a class="skip" href="#main">Skip to controller</a>' +
                   parent_links(header) + template.read_text() + parent_links(footer) +
                   '<script type="module" src="controller.js"></script></body></html>')
template.unlink()


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.refs = []
        self.ids = set()

    def handle_starttag(self, tag, attrs):
        attr = dict(attrs)
        if attr.get("id"):
            self.ids.add(attr["id"])
        self.refs.extend(attr[k] for k in ("href", "src") if attr.get(k))


pages = {}
for page in (out / "index.html", out / "facets.html", runtime):
    links = Links()
    links.feed(page.read_text())
    pages[page] = links
for page, links in pages.items():
    for ref in links.refs:
        url = urlsplit(ref)
        if url.scheme or url.netloc:
            continue
        dest = (page.parent / unquote(url.path)).resolve() if url.path else page
        if dest.is_dir():
            dest /= "index.html"
        if not dest.is_relative_to(out) or not dest.is_file():
            raise ValueError(f"{page.name}: missing local resource {ref}")
        if url.fragment and dest in pages and url.fragment not in pages[dest].ids:
            raise ValueError(f"missing fragment {ref}")
print(f"Published files ready for {version}, model {record['holo']}, {len(groups)} facets")
