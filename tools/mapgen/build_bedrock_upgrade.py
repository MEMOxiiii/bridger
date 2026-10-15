#!/usr/bin/env python3
"""Builds src/translate/data/bedrock-upgrade.nbt: the data that upgrades block states found in
old Bedrock worlds and structures to the pinned Bedrock palette (docs/formats/bedrock-world.md,
"Old worlds").

Source: opencollab-incubator/BedrockBlockUpgradeSchema (CC0-1.0, formerly pmmp/BedrockBlockUpgradeSchema),
pinned by commit below:
  nbt_upgrade_schema/*.json + 0351_1.26.40_to_1.26.50.json   state upgrade schemas, one per game version step
  block_legacy_id_map.json                                   numeric block id <-> string id (up to 1.16)
  id_meta_to_nbt/1.12.0.bin                                  (string id, meta) -> 1.12 block state NBT

The output is one little-endian NBT compound, so Bridger reads it with its own bounds-checked NBT
parser (no JSON at run time):
  schemas:        list of {id, version, renamed_ids, renamed_properties, added_properties,
                  removed_properties, remapped_values, flattened, remapped_states}, in application order
  legacy_ids:     {string id: Int numeric id}
  legacy_states:  {string id: list of {meta: Int, name: String, states: Compound, version: Int}}

    python tools/mapgen/build_bedrock_upgrade.py
"""

import io
import json
import re
import struct
import sys
import urllib.request
from pathlib import Path

COMMIT = "4e874a162c4d41a9db7df72980e97849d6942d5d"  # "Bedrock 1.26.50", 2026-09-18
BASE = f"https://raw.githubusercontent.com/opencollab-incubator/BedrockBlockUpgradeSchema/{COMMIT}/"
API = f"https://api.github.com/repos/opencollab-incubator/BedrockBlockUpgradeSchema/contents/nbt_upgrade_schema?ref={COMMIT}"
# Committed at the repository root instead of nbt_upgrade_schema/ in this commit; listed explicitly.
EXTRA_SCHEMAS = ["0351_1.26.40_to_1.26.50.json"]
OUT = Path(__file__).resolve().parents[2] / "src" / "translate" / "data" / "bedrock-upgrade.nbt"


def fetch(path):
    return urllib.request.urlopen(BASE + path, timeout=120).read()


# ---- Little-endian NBT (only what this file needs) -------------------------------------------
END, BYTE, SHORT, INT, LONG, FLOAT, DOUBLE, BARR, STRING, LIST, COMPOUND, IARR, LARR = range(13)


class Tag:
    def __init__(self, kind, value):
        self.kind, self.value = kind, value


def b(v): return Tag(BYTE, v)
def i(v): return Tag(INT, v)
def s(v): return Tag(STRING, v)
def c(d): return Tag(COMPOUND, d)          # dict name -> Tag
def lst(kind, items): return Tag(LIST, (kind, items))


def w_str(out, text):
    raw = text.encode("utf-8")
    out.write(struct.pack("<H", len(raw)))
    out.write(raw)


def w_payload(out, t):
    k, v = t.kind, t.value
    if k == BYTE: out.write(struct.pack("<b", v))
    elif k == SHORT: out.write(struct.pack("<h", v))
    elif k == INT: out.write(struct.pack("<i", v))
    elif k == LONG: out.write(struct.pack("<q", v))
    elif k == STRING: w_str(out, v)
    elif k == LIST:
        kind, items = v
        out.write(struct.pack("<bi", kind if items else END, len(items)))
        for it in items: w_payload(out, it)
    elif k == COMPOUND:
        for name, child in v.items():
            out.write(struct.pack("<b", child.kind))
            w_str(out, name)
            w_payload(out, child)
        out.write(b"\0")
    else:
        raise ValueError(f"tag type {k} not written by this script")


def r_payload(buf, p, k):
    if k == BYTE: return Tag(BYTE, struct.unpack_from("<b", buf, p)[0]), p + 1
    if k == SHORT: return Tag(SHORT, struct.unpack_from("<h", buf, p)[0]), p + 2
    if k == INT: return Tag(INT, struct.unpack_from("<i", buf, p)[0]), p + 4
    if k == LONG: return Tag(LONG, struct.unpack_from("<q", buf, p)[0]), p + 8
    if k == STRING:
        n = struct.unpack_from("<H", buf, p)[0]
        return Tag(STRING, buf[p + 2:p + 2 + n].decode("utf-8")), p + 2 + n
    if k == LIST:
        kind, n = struct.unpack_from("<bi", buf, p)
        p += 5
        items = []
        for _ in range(n):
            t, p = r_payload(buf, p, kind)
            items.append(t)
        return Tag(LIST, (kind, items)), p
    if k == COMPOUND:
        d = {}
        while True:
            kind = buf[p]
            p += 1
            if kind == END: return Tag(COMPOUND, d), p
            n = struct.unpack_from("<H", buf, p)[0]
            name = buf[p + 2:p + 2 + n].decode("utf-8")
            t, p = r_payload(buf, p + 2 + n, kind)
            d[name] = t
    raise ValueError(f"unexpected tag type {k}")


def r_root(buf, p):
    if buf[p] != COMPOUND: raise ValueError("root is not a compound")
    n = struct.unpack_from("<H", buf, p + 1)[0]
    return r_payload(buf, p + 3 + n, COMPOUND)


def varint(buf, p):
    v = shift = 0
    while True:
        x = buf[p]
        p += 1
        v |= (x & 0x7F) << shift
        if not x & 0x80: return v, p
        shift += 7


# ---- Schema JSON -> NBT ------------------------------------------------------------------------
def prop(j):
    """blockStateProperty {"int"|"string"|"byte": value} -> typed tag."""
    (k, v), = j.items()
    return {"int": i, "string": s, "byte": b}[k](v)


def flatten_rule(j):
    d = {"prefix": s(j["prefix"]), "property": s(j["flattenedProperty"]),
         "type": s(j.get("flattenedPropertyType", "string")), "suffix": s(j["suffix"])}
    if "flattenedValueRemaps" in j:
        d["remaps"] = c({k: s(v) for k, v in j["flattenedValueRemaps"].items()})
    return c(d)


def schema_to_nbt(schema_id, j):
    version = (j["maxVersionMajor"] << 24) | (j["maxVersionMinor"] << 16) | (j["maxVersionPatch"] << 8) | j["maxVersionRevision"]
    d = {"id": i(schema_id), "version": i(version)}
    if "renamedIds" in j:
        d["renamed_ids"] = c({k: s(v) for k, v in j["renamedIds"].items()})
    if "renamedProperties" in j:
        d["renamed_properties"] = c({blk: c({k: s(v) for k, v in m.items()}) for blk, m in j["renamedProperties"].items()})
    if "addedProperties" in j:
        d["added_properties"] = c({blk: c({k: prop(v) for k, v in m.items()}) for blk, m in j["addedProperties"].items()})
    if "removedProperties" in j:
        d["removed_properties"] = c({blk: lst(STRING, [s(x) for x in v]) for blk, v in j["removedProperties"].items()})
    if "remappedPropertyValues" in j:
        index = j.get("remappedPropertyValuesIndex", {})
        d["remapped_values"] = c({
            blk: c({p: lst(COMPOUND, [c({"old": prop(pair["old"]), "new": prop(pair["new"])}) for pair in index[key]])
                    for p, key in m.items()})
            for blk, m in j["remappedPropertyValues"].items()})
    if "flattenedProperties" in j:
        d["flattened"] = c({blk: flatten_rule(r) for blk, r in j["flattenedProperties"].items()})
    if "remappedStates" in j:
        out = {}
        for blk, rules in j["remappedStates"].items():
            items = []
            for r in rules:
                e = {"new": c({k: prop(v) for k, v in (r.get("newState") or {}).items()})}
                e["old"] = c({k: prop(v) for k, v in (r.get("oldState") or {}).items()})
                if "newName" in r: e["new_name"] = s(r["newName"])
                else: e["new_flattened"] = flatten_rule(r["newFlattenedName"])
                e["copied"] = lst(STRING, [s(x) for x in r.get("copiedState", [])])
                items.append(c(e))
            out[blk] = lst(COMPOUND, items)
        d["remapped_states"] = c(out)
    return version, c(d)


def main():
    listing = json.load(urllib.request.urlopen(API, timeout=120))
    names = sorted([e["name"] for e in listing if e["name"].endswith(".json")] + EXTRA_SCHEMAS)
    schemas = []
    for name in names:
        m = re.match(r"(\d{4})_", name)
        if not m: sys.exit(f"unexpected schema file name {name}")
        path = name if name in EXTRA_SCHEMAS else "nbt_upgrade_schema/" + name
        version, tag = schema_to_nbt(int(m.group(1)), json.loads(fetch(path)))
        schemas.append((version, int(m.group(1)), tag))
        print(f"schema {name}")
    schemas.sort(key=lambda t: (t[0], t[1]))  # application order: version id, then schema id

    legacy_ids = json.loads(fetch("block_legacy_id_map.json"))
    bin_data = fetch("id_meta_to_nbt/1.12.0.bin")
    p = 0
    count, p = varint(bin_data, p)
    legacy_states = {}
    for _ in range(count):
        n, p = varint(bin_data, p)
        sid = bin_data[p:p + n].decode("utf-8")
        p += n
        metas, p = varint(bin_data, p)
        items = []
        for _ in range(metas):
            meta, p = varint(bin_data, p)
            state, p = r_root(bin_data, p)
            st = state.value
            items.append(c({"meta": i(meta), "name": st["name"], "states": st.get("states", c({})),
                            "version": st.get("version", i(0))}))
        legacy_states[sid] = lst(COMPOUND, items)
    if p != len(bin_data): sys.exit("trailing data in 1.12.0.bin")

    root = c({"source": s(f"opencollab-incubator/BedrockBlockUpgradeSchema@{COMMIT} (CC0-1.0)"),
              "schemas": lst(COMPOUND, [t for _, _, t in schemas]),
              "legacy_ids": c({k: i(v) for k, v in legacy_ids.items()}),
              "legacy_states": c(legacy_states)})
    out = io.BytesIO()
    out.write(struct.pack("<b", COMPOUND))
    w_str(out, "")
    w_payload(out, root)
    OUT.write_bytes(out.getvalue())
    print(f"wrote {OUT} ({len(out.getvalue())} bytes, {len(schemas)} schemas, {len(legacy_states)} legacy ids)")


if __name__ == "__main__":
    main()
