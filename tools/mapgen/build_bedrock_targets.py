#!/usr/bin/env python3
"""Prepares the Bedrock target versions (docs/versions.md, "Target versions"):

1. Downloads every block palette of opencollab-incubator/BedrockBlockPaletteArchive (CC0, pinned commit) from 1.18
   on, converts it from the network NBT the archive uses (varint lengths, zigzag varint ints) to standard
   little-endian NBT, and writes tools/mapgen/sources/cache/bedrock-palettes/<file>.nbt as {blocks: [...]}.
2. Runs bridger_targetgen (tools/mapgen/targetgen.cpp), which upgrades every state of each target's palette with
   Bridger's own upgrader and writes src/translate/data/bedrock-targets.nbt.

    python tools/mapgen/build_bedrock_targets.py <path to bridger_targetgen.exe>
"""

import io
import json
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CACHE = HERE / "sources" / "cache" / "bedrock-palettes"
COMMIT = "5d95b15eb5e9407d486fad9329315fec571f961d"
# Item renames per version: opencollab-incubator/BedrockItemUpgradeSchema (CC0), pinned.
ITEM_COMMIT = "7f551f625fee3be0c8d33be1fee295c7ec6a277e"
ITEM_API = f"https://api.github.com/repos/opencollab-incubator/BedrockItemUpgradeSchema/contents/id_meta_upgrade_schema?ref={ITEM_COMMIT}"
ITEM_RAW = f"https://raw.githubusercontent.com/opencollab-incubator/BedrockItemUpgradeSchema/{ITEM_COMMIT}/id_meta_upgrade_schema/"
API = f"https://api.github.com/repos/opencollab-incubator/BedrockBlockPaletteArchive/contents/?ref={COMMIT}"
RAW = f"https://raw.githubusercontent.com/opencollab-incubator/BedrockBlockPaletteArchive/{COMMIT}/"


def fetch(url):
    for attempt in range(4):
        try:
            return urllib.request.urlopen(url, timeout=120).read()
        except OSError as e:
            if attempt == 3:
                raise
            print(f"retrying {url}: {e}", flush=True)


def version_of(file):
    return tuple(int(x) for x in file[:-4].split("_")[0].split("."))


# ---- network NBT (read) -> little-endian NBT (write) --------------------------------------------
class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def uvar(self):
        v = shift = 0
        while True:
            x = self.byte()
            v |= (x & 0x7F) << shift
            if not x & 0x80:
                return v
            shift += 7

    def svar(self):
        u = self.uvar()
        return (u >> 1) ^ -(u & 1)

    def string(self):
        n = self.uvar()
        s = self.b[self.p:self.p + n]
        self.p += n
        return s


def convert_payload(r, kind, o):
    """Reads one network-NBT payload of `kind` and writes it as little-endian NBT payload."""
    if kind == 1: o.write(struct.pack("<b", struct.unpack("<b", bytes([r.byte()]))[0]))
    elif kind == 2: o.write(r.b[r.p:r.p + 2]); r.p += 2
    elif kind == 3: o.write(struct.pack("<i", r.svar()))
    elif kind == 4: o.write(struct.pack("<q", r.svar()))
    elif kind == 5: o.write(r.b[r.p:r.p + 4]); r.p += 4
    elif kind == 6: o.write(r.b[r.p:r.p + 8]); r.p += 8
    elif kind == 7:
        n = r.svar(); o.write(struct.pack("<i", n)); o.write(r.b[r.p:r.p + n]); r.p += n
    elif kind == 8:
        s = r.string(); o.write(struct.pack("<H", len(s))); o.write(s)
    elif kind == 9:
        el = r.byte(); n = r.svar()
        o.write(struct.pack("<bi", el, n))
        for _ in range(n): convert_payload(r, el, o)
    elif kind == 10:
        while True:
            t = r.byte()
            o.write(struct.pack("<b", t))
            if t == 0: return
            s = r.string(); o.write(struct.pack("<H", len(s))); o.write(s)
            convert_payload(r, t, o)
    elif kind == 11:
        n = r.svar(); o.write(struct.pack("<i", n))
        for _ in range(n): o.write(struct.pack("<i", r.svar()))
    elif kind == 12:
        n = r.svar(); o.write(struct.pack("<i", n))
        for _ in range(n): o.write(struct.pack("<q", r.svar()))
    else:
        raise ValueError(f"tag type {kind}")


def convert_palette(data):
    r = Reader(data)
    entries = []
    while r.p < len(data):
        if r.byte() != 10: raise ValueError("palette entry is not a compound")
        r.string()  # root name
        o = io.BytesIO()
        convert_payload(r, 10, o)
        entries.append(o.getvalue())
    out = io.BytesIO()
    out.write(b"\x0a\x00\x00")                  # root compound, empty name
    out.write(b"\x09"); out.write(struct.pack("<H", 6)); out.write(b"blocks")
    out.write(struct.pack("<bi", 10, len(entries)))
    for e in entries: out.write(e)
    out.write(b"\x00")
    return out.getvalue(), len(entries)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    CACHE.mkdir(parents=True, exist_ok=True)
    files = sorted((e["name"] for e in json.loads(fetch(API)) if e["name"].endswith(".nbt") and version_of(e["name"]) >= (1, 18)),
                   key=version_of)
    for f in files:
        dest = CACHE / f
        if dest.exists():
            continue
        data, n = convert_palette(fetch(RAW + f))
        dest.write_bytes(data)
        print(f"{f}: {n} states", flush=True)
    # Item schemas, in order; each with the version it upgrades to ("0091_1.19.60_to_1.19.70.26_beta" -> 1.19.70.26).
    items = []
    for e in sorted(json.loads(fetch(ITEM_API)), key=lambda e: e["name"]):
        if not e["name"].endswith(".json"): continue
        to = e["name"][:-5].split("_to_")[1].split("_")[0]
        schema = json.loads(fetch(ITEM_RAW + e["name"]))
        items.append({"to": [int(x) for x in to.split(".")], "renamed": schema.get("renamedIds", {}),
                      "remapped": schema.get("remappedMetas", {})})
    item_json = CACHE / "item_schemas.json"
    item_json.write_text(json.dumps(items))
    print(f"{len(items)} item schemas", flush=True)
    subprocess.run([sys.argv[1], "--palettes", str(CACHE), "--targets", str(HERE / "overrides" / "bedrock_targets.json"),
                    "--items", str(item_json),
                    "--out", str(ROOT / "src" / "translate" / "data" / "bedrock-targets.nbt"),
                    "--source", f"opencollab-incubator/BedrockBlockPaletteArchive@{COMMIT}, BedrockItemUpgradeSchema@{ITEM_COMMIT} (CC0-1.0)"],
                   check=True)


if __name__ == "__main__":
    main()
