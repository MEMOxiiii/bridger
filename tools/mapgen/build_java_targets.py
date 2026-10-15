#!/usr/bin/env python3
"""Builds src/translate/data/java-targets.nbt: the block states of every Java release Bridger can write
(docs/versions.md, "Target versions"), so a conversion can target an older release than the pinned one.

For each release from 1.18 to the pinned version, the official server jar is downloaded from Mojang (URL and
SHA-1 from Mojang's version manifest) and Mojang's own data generator writes reports/blocks.json, exactly as
fetch_sources.py does for the pinned version. Jars and reports stay in tools/mapgen/sources/cache/ (gitignored);
only the condensed block lists are committed (names, properties and values: facts about the game, as in the
.brmap). Requires Java 21+ on PATH.

Output (little-endian NBT, read with Bridger's own parser):
  versions: list of {name: String, data_version: Int}, oldest first
  blocks:   {block id: list of {from: Int, to: Int, props: Compound{name: List<String> values},
                                  default: Compound{name: String value}}}
            one entry per run of consecutive versions (indices into `versions`, inclusive) with the same definition
  items:    {item id: IntArray of (from, to) pairs}: the versions that have the item (registries.json)

    python tools/mapgen/build_java_targets.py
"""

import hashlib
import io
import json
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE / "sources" / "cache" / "java-targets"
OUT = HERE.parents[1] / "src" / "translate" / "data" / "java-targets.nbt"
MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
FIRST, LAST = "1.18", "26.3"
ALLOWED_HOSTS = ("https://piston-meta.mojang.com/", "https://piston-data.mojang.com/", "https://launcher.mojang.com/")


def fetch(url):
    if not url.startswith(ALLOWED_HOSTS):
        sys.exit(f"refusing to download from {url}")
    for attempt in range(4):  # Mojang's CDN occasionally stalls
        try:
            return urllib.request.urlopen(url, timeout=300).read()
        except OSError as e:
            if attempt == 3:
                raise
            print(f"retrying {url}: {e}", flush=True)


def releases():
    man = json.loads(fetch(MANIFEST))
    rel = [v for v in man["versions"] if v["type"] == "release"]
    ids = [v["id"] for v in rel]
    first, last = ids.index(FIRST), ids.index(LAST)
    return list(reversed(rel[last:first + 1]))  # oldest first


def item_ids(version):
    reg = json.loads((CACHE / version["id"] / "reports" / "registries.json").read_text())
    return sorted(reg["minecraft:item"]["entries"].keys())


def blocks_report(version):
    out = CACHE / version["id"]
    report = out / "reports" / "blocks.json"
    if report.exists() and (out / "version.json").exists():
        return json.loads(report.read_text()), json.loads((out / "version.json").read_text())
    meta = fetch(version["url"])
    if hashlib.sha1(meta).hexdigest() != version["sha1"]:
        sys.exit(f"{version['id']}: version file hash mismatch")
    server = json.loads(meta)["downloads"]["server"]
    jar = CACHE / f"server-{version['id']}.jar"
    if not jar.exists() or hashlib.sha1(jar.read_bytes()).hexdigest() != server["sha1"]:
        data = fetch(server["url"])
        if hashlib.sha1(data).hexdigest() != server["sha1"]:
            sys.exit(f"{version['id']}: server jar hash mismatch")
        CACHE.mkdir(parents=True, exist_ok=True)
        jar.write_bytes(data)
    print(f"{version['id']}: running Mojang's data generator", flush=True)
    subprocess.run(["java", "-DbundlerMainClass=net.minecraft.data.Main", "-jar", str(jar), "--reports", "--output", str(out)],
                   cwd=CACHE, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import zipfile
    with zipfile.ZipFile(jar) as z:
        (out / "version.json").write_bytes(z.read("version.json"))
    jar.unlink()  # keep only the reports
    return json.loads(report.read_text()), json.loads((out / "version.json").read_text())


# ---- Little-endian NBT writer ------------------------------------------------------------------
def w_str(o, s):
    b = s.encode("utf-8")
    o.write(struct.pack("<H", len(b)))
    o.write(b)


def w_tag(o, kind, v):
    if kind == "int": o.write(struct.pack("<i", v))
    elif kind == "str": w_str(o, v)
    elif kind == "list_str":
        o.write(struct.pack("<bi", 8 if v else 0, len(v)))
        for x in v: w_str(o, x)
    elif kind == "list_compound":
        o.write(struct.pack("<bi", 10 if v else 0, len(v)))
        for c in v: w_compound(o, c)
    elif kind == "compound": w_compound(o, v)
    elif kind == "int_array":
        o.write(struct.pack("<i", len(v)))
        for x in v: o.write(struct.pack("<i", x))


KIND_ID = {"int": 3, "str": 8, "list_str": 9, "list_compound": 9, "compound": 10, "int_array": 11}


def w_compound(o, d):
    for name, (kind, v) in d.items():
        o.write(struct.pack("<b", KIND_ID[kind]))
        w_str(o, name)
        w_tag(o, kind, v)
    o.write(b"\0")


def definition(b):
    props = b.get("properties", {})
    default = next((s.get("properties", {}) for s in b["states"] if s.get("default")), {})
    return json.dumps([props, default], sort_keys=True)


def main():
    vers = releases()
    defs = []  # per version: {block: definition json}
    names = []
    items = []  # per version: set of item ids
    for v in vers:
        report, vj = blocks_report(v)
        items.append(set(item_ids(v)))
        names.append((v["id"], vj["world_version"]))
        defs.append({k: definition(b) for k, b in report.items()})
        print(f"{v['id']}: DataVersion {vj['world_version']}, {len(report)} blocks", flush=True)
    blocks = {}
    for block in sorted(set().union(*defs)):
        runs = []
        for i, d in enumerate(defs):
            cur = d.get(block)
            if cur is None: continue
            if runs and runs[-1][1] == i - 1 and runs[-1][2] == cur:
                runs[-1][1] = i
            else:
                runs.append([i, i, cur])
        entries = []
        for a, b, js in runs:
            props, default = json.loads(js)
            entries.append({"from": ("int", a), "to": ("int", b),
                            "props": ("compound", {k: ("list_str", vals) for k, vals in props.items()}),
                            "default": ("compound", {k: ("str", val) for k, val in default.items()})})
        blocks[block] = ("list_compound", entries)
    item_runs = {}
    for item in sorted(set().union(*items)):
        runs = []
        for i, have in enumerate(items):
            if item not in have: continue
            if runs and runs[-1][1] == i - 1: runs[-1][1] = i
            else: runs.append([i, i])
        item_runs[item] = ("int_array", [x for r in runs for x in r])
    root = {"versions": ("list_compound", [{"name": ("str", n), "data_version": ("int", dv)} for n, dv in names]),
            "blocks": ("compound", blocks), "items": ("compound", item_runs)}
    o = io.BytesIO()
    o.write(b"\x0a")
    w_str(o, "")
    w_compound(o, root)
    OUT.write_bytes(o.getvalue())
    print(f"wrote {OUT} ({len(o.getvalue())} bytes, {len(names)} versions, {len(blocks)} block ids)")


if __name__ == "__main__":
    main()
