"""Render the four design projections from their Prism declarations."""
from html import escape
import json
import re
import subprocess
from xml.etree import ElementTree as ET


def sequence_svg(path, title, lifelines, messages, label):
    lanes = {key: 110 + i * 225 for i, (order, key) in enumerate(sorted(lifelines))}
    width, height = max(lanes.values()) + 110, 100 + len(messages) * 70
    svg = ET.Element("svg", xmlns="http://www.w3.org/2000/svg", viewBox=f"0 0 {width} {height}")
    ET.SubElement(svg, "title").text = title
    group = ET.SubElement(svg, "g", fill="#103b30", attrib={"font-family": "sans-serif", "font-size": "14"})
    for lane, x in lanes.items():
        ET.SubElement(group, "rect", x=str(x-103), y="8", width="206", height="32", rx="4", fill="#e0efdc", stroke="#366449")
        ET.SubElement(group, "text", x=str(x), y="29", attrib={"text-anchor": "middle"}).text = label(lane)
        ET.SubElement(group, "path", d=f"M{x} 40 V{height-10}", stroke="#527561", attrib={"stroke-dasharray":"5 4"})
    for i, (order, src, dest, relation) in enumerate(sorted(messages)):
        x, tip, y = lanes[src], lanes[dest], 92 + i * 70
        back = tip - 8 if tip > x else tip + 8
        ET.SubElement(group, "path", d=f"M{x} {y} H{tip}", stroke="#366449")
        ET.SubElement(group, "path", d=f"M{tip} {y} L{back} {y-4} V{y+4} Z")
        ET.SubElement(group, "text", x=str((x+tip)//2), y=str(y-9), attrib={"text-anchor":"middle"}).text = relation
    ET.ElementTree(svg).write(path, encoding="utf-8", xml_declaration=True)


def design_views(groups, directory):
    records = {r["id"]: r for group in groups for r in group["records"]}
    by_index = {r["fields"]["index"]: r["id"] for r in records.values()
                if r["kind"] == "Foundation.Arch::Component"}
    directory.mkdir()

    def label(ref):
        fields = records[ref]["fields"]
        if "name" in fields:
            return fields["name"]
        short = re.sub(r"^(state|process)", "", ref.split("::")[-1])
        return re.sub(r"([a-z])([A-Z])", r"\1 \2", short).capitalize()

    view_html = []
    for view in records.values():
        if view["kind"] != "Foundation.Arch::DesignView":
            continue
        title = view["fields"]["title"]
        nodes = {}
        edges = []
        lifelines = []
        messages = []
        initial = []
        for rec in records.values():
            f = rec["fields"]
            if f.get("view") != view["id"]:
                continue
            kind = rec["kind"].split("::")[-1]
            if kind == "OpmThing":
                nodes[f["subject"]] = "ellipse" if f["kind"] == "process" else "box"
            elif kind == "OpmLink":
                edges.append((f["source"], f["target"], f["kind"]))
            elif kind == "OpmZoom":
                edges.append((f["process"], f["subprocess"], "in-zoom"))
            elif kind == "OpmState":
                nodes[f["state"]] = "oval"
                edges.append((f["thing"], f["state"], "has state"))
            elif kind in ("UmlComponent", "UmlLifeline"):
                nodes[f["component"]] = "box"
                if kind == "UmlLifeline":
                    lifelines.append((f["order"], f["component"]))
            elif kind == "UmlConnector":
                edge = records[f["edge"]]["fields"]
                relation = re.sub(r"([a-z])([A-Z])", r"\1 \2", edge["kind"]).lower()
                edges.append((by_index[edge["fromIndex"]], by_index[edge["toIndex"]], relation))
            elif kind == "UmlMessage":
                edges.append((f["source"], f["target"], f'{f["order"]+1}. {label(f["process"])} ({f["kind"]})'))
                messages.append((f["order"], *edges[-1]))
            elif kind == "UmlState":
                nodes[f["state"]] = "ellipse"
                if f["initial"]:
                    initial.append(f["state"])
            elif kind == "UmlTransition":
                edges.append((f["source"], f["target"], label(f["trigger"])))
        lines = ['digraph {', 'graph [bgcolor="transparent", pad=.2, rankdir=TB];',
                 'node [fontname="sans-serif", fontsize=14, style=filled, color="#366449", fillcolor="#e0efdc", fontcolor="#103b30"];',
                 'edge [fontname="sans-serif", fontsize=12, color="#527561", fontcolor="#254b36", arrowsize=.75];']
        q = json.dumps
        for key, shape in nodes.items():
            lines.append(f'{q(key)} [label={q(label(key))}, shape={shape}];')
        for state in initial:
            lines.extend(['start [shape=point,fillcolor="#103b30",width=.12];', f'start -> {q(state)};'])
        for src, dest, relation in edges:
            lines.append(f'{q(src)} -> {q(dest)} [label={q(relation)}];')
        slug = view["fields"]["diagram"] + ".svg"
        if lifelines:
            sequence_svg(directory / slug, title, lifelines, messages, label)
        else:
            subprocess.run(["dot", "-Tsvg", "-o", directory / slug], input="\n".join(lines + ["}"]), text=True, check=True)
        links = "".join(f'<li>{escape(label(a))} → {escape(label(b))} ({escape(rel)})</li>' for a,b,rel in edges)
        view_html.append(f'<figure class="design-view"><figcaption>{escape(title)}</figcaption>'
                         f'<a href="diagrams/{slug}"><img loading="lazy" src="diagrams/{slug}" alt="{escape(title)} diagram. Relationships are listed below."></a>'
                         f'<details><summary>Read the relationships</summary><ul>{links}</ul></details></figure>')
    return "".join(view_html)
