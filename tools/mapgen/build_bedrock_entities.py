#!/usr/bin/env python3
"""Builds src/translate/data/bedrock-entities.nbt: how Bedrock 26.50 describes the state of each mob in a saved actor's
`definitions` list (docs/formats/bedrock-world.md, "Mobs").

Bedrock saves, next to an actor's plain tags (Variant, Color, IsBaby, ...), the component groups its behavior file added
to it, e.g. ["+minecraft:sheep", "+minecraft:sheep_adult", "+minecraft:sheep_dyeable", ..., "+minecraft:sheep_gray"].
Those groups come from the mob's spawn event in Mojang's vanilla behavior pack. This script walks every outcome of each
mob's "minecraft:entity_spawned" (and "minecraft:entity_born") event: `randomize` and `first_valid` branch, `sequence`
runs in order, `trigger` follows the named event, filters are not evaluated (every option counts as possible). For each
outcome it records the groups in the order they were added and the state they give the mob (variant, mark variant,
colors, skin, baby, tamed, saddled, chested, sheared) and how likely that outcome is. Bridger picks the outcome matching
the mob it converts.

Source: Mojang/bedrock-samples (behavior_pack/entities/*.json), tag v1.26.50.4, pinned by commit and by one SHA-256
over all files. The files are only read here; nothing of them is committed except the derived table.

    python tools/mapgen/build_bedrock_entities.py [--check defs.json]

--check: a dump of definitions found in real worlds (scratch tool, not committed): each is compared with the outcome
Bridger would choose for the same state, and differences are printed.
"""

import hashlib
import io
import json
import re
import struct
import sys
import urllib.request
from pathlib import Path

TAG = "v1.26.50.4"
COMMIT = "46ba6ea985fb5a92d79a9419198f10dda14c199d"  # Mojang/bedrock-samples, 2026-09-16
FILES_SHA256 = "1530ec5953d8a57ef3822824d910bb6c38ea835c22dddb22b285d9e63c384065"  # over "name\0bytes" of each file, sorted
API = f"https://api.github.com/repos/Mojang/bedrock-samples/contents/behavior_pack/entities?ref={COMMIT}"
RAW = f"https://raw.githubusercontent.com/Mojang/bedrock-samples/{COMMIT}/behavior_pack/entities/"
HERE = Path(__file__).resolve().parent
CACHE = HERE / "sources" / "cache" / f"bedrock-samples-{TAG}" / "entities"
OUT = HERE.parents[1] / "src" / "translate" / "data" / "bedrock-entities.nbt"
MAX_OUTCOMES = 4096  # villagers: 6 skins x 15 professions x 7 biomes x adult/baby
PROPS = {  # component -> outcome field
    "minecraft:variant": "variant",
    "minecraft:mark_variant": "mark_variant",
    "minecraft:color": "color",
    "minecraft:color2": "color2",
    "minecraft:skin_id": "skin_id",
}
FLAGS = {"minecraft:is_baby": "baby", "minecraft:is_tamed": "tamed", "minecraft:is_saddled": "saddled",
         "minecraft:is_chested": "chested", "minecraft:is_sheared": "sheared"}


# ---- Little-endian NBT (only what this file needs; as in build_bedrock_upgrade.py) -------------------------------
END, BYTE, SHORT, INT, LONG, FLOAT, DOUBLE, BARR, STRING, LIST, COMPOUND = range(11)


class Tag:
    def __init__(self, kind, value):
        self.kind, self.value = kind, value


def b(v): return Tag(BYTE, v)
def i(v): return Tag(INT, v)
def s(v): return Tag(STRING, v)
def c(d): return Tag(COMPOUND, d)
def lst(kind, items): return Tag(LIST, (kind, items))


def w_str(out, text):
    raw = text.encode("utf-8")
    out.write(struct.pack("<H", len(raw)))
    out.write(raw)


def w_payload(out, t):
    k, v = t.kind, t.value
    if k == BARR:
        out.write(struct.pack("<i", len(v)))
        out.write(bytes(v))
        return
    if k == BYTE: out.write(struct.pack("<b", v))
    elif k == INT: out.write(struct.pack("<i", v))
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


# ---- Sources ------------------------------------------------------------------------------------------------------
def fetch_files():
    CACHE.mkdir(parents=True, exist_ok=True)
    names = sorted(p.name for p in CACHE.glob("*.json"))
    if not names:
        listing = json.loads(urllib.request.urlopen(API, timeout=60).read())
        for entry in listing:
            if entry["name"].endswith(".json"):
                (CACHE / entry["name"]).write_bytes(urllib.request.urlopen(RAW + entry["name"], timeout=60).read())
        names = sorted(p.name for p in CACHE.glob("*.json"))
    digest = hashlib.sha256()
    for n in names:
        digest.update(n.encode() + b"\0" + (CACHE / n).read_bytes())
    if FILES_SHA256 != "PIN" and digest.hexdigest() != FILES_SHA256:
        sys.exit(f"bedrock-samples files changed: {digest.hexdigest()} != {FILES_SHA256} (delete {CACHE} to fetch again)")
    print(f"{len(names)} entity files, sha256 {digest.hexdigest()}")
    return {n: (CACHE / n).read_text(encoding="utf-8") for n in names}


def parse(text):
    """Mojang's files are JSON with // and /* */ comments and the odd trailing comma."""
    out, k, n, in_str = [], 0, len(text), False
    while k < n:
        ch = text[k]
        if in_str:
            out.append(ch)
            if ch == "\\" and k + 1 < n:
                out.append(text[k + 1])
                k += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            out.append(ch)
        elif text.startswith("//", k):
            while k < n and text[k] != "\n":
                k += 1
            continue
        elif text.startswith("/*", k):
            end = text.find("*/", k + 2)
            k = n if end < 0 else end + 2
            continue
        else:
            out.append(ch)
        k += 1
    return json.loads(re.sub(r",(\s*[}\]])", r"\1", "".join(out)))


# ---- Event outcomes -------------------------------------------------------------------------------------------------
def components_of(entity, groups):
    comps = set((entity.get("components") or {}).keys())
    cg = entity.get("component_groups") or {}
    for g in groups:
        comps |= set((cg.get(g) or {}).keys())
    return comps


def test_filter(f, comps):
    """True/False when the filter can be decided at spawn time, None when it depends on the world (biome, time...).
    Decided: has_component (the mob's own components) and is_difficulty (assumed Normal, the usual survival setting)."""
    if isinstance(f, list):
        f = {"all_of": f}
    if not isinstance(f, dict):
        return None
    for key, combine in (("all_of", all), ("any_of", any)):
        if key in f:
            vals = [test_filter(x, comps) for x in f[key]]
            if any(v is None for v in vals):
                return None
            return combine(vals)
    if "none_of" in f:
        vals = [test_filter(x, comps) for x in f["none_of"]]
        return None if any(v is None for v in vals) else not any(vals)
    test, value = f.get("test"), f.get("value")
    negate = f.get("operator") in ("!=", "not", "<>")
    if test == "has_component":
        res = value in comps
    elif test == "is_difficulty":
        res = value == "normal"
    else:
        return None
    return (not res) if negate else res


def run(entity, action, states, depth=0):
    """Applies an event action to every state; a state is (groups tuple, weight)."""
    if depth > 12 or not isinstance(action, dict):
        return states
    if "filters" in action:
        # Decided per state; an undecidable condition (biome, time...) may or may not hold. Not running is preferred
        # when nothing else decides, so a branch that ran on an unknown condition gets half the weight.
        run_full, run_half, skip = [], [], []
        for g, p in states:
            v = test_filter(action["filters"], components_of(entity, g))
            (run_full if v is True else run_half if v is None else skip).append((g, p))
        body = {k: v for k, v in action.items() if k != "filters"}
        out = run(entity, body, run_full, depth + 1) if run_full else []
        if run_half:
            out += [(g, p * 0.5) for g, p in run(entity, body, run_half, depth + 1)] + run_half
        return merge(out + skip)
    events = entity.get("events") or {}
    out = states
    rem = action.get("remove", {}).get("component_groups", []) if isinstance(action.get("remove"), dict) else []
    add = action.get("add", {}).get("component_groups", []) if isinstance(action.get("add"), dict) else []
    if rem or add:
        nxt = []
        for groups, p in out:
            g = [x for x in groups if x not in rem]
            for a in add:
                if a not in g:
                    g.append(a)
            nxt.append((tuple(g), p))
        out = nxt
    if "sequence" in action:
        for step in action["sequence"]:
            out = run(entity, step, out, depth + 1)
    if "randomize" in action:
        options = action["randomize"]
        total = sum(o.get("weight", 1) for o in options) or 1
        nxt = []
        for o in options:
            nxt += run(entity, o, [(g, p * o.get("weight", 1) / total) for g, p in out], depth + 1)
        out = nxt
    if "first_valid" in action:
        # The first option whose filters hold runs. Per state: options decided false are passed over, the first one
        # decided true ends the search, undecidable ones branch (at half weight) and the search goes on.
        nxt = []
        for g, p in out:
            for o in action["first_valid"]:
                f = o.get("filters") if isinstance(o, dict) else None
                v = True if f is None else test_filter(f, components_of(entity, g))
                if v is False:
                    continue
                body = {k: x for k, x in o.items() if k != "filters"} if isinstance(o, dict) else o
                if v is True:
                    nxt += run(entity, body, [(g, p)], depth + 1)
                    break
                nxt += [(h, q * 0.5) for h, q in run(entity, body, [(g, p)], depth + 1)]
            else:
                nxt.append((g, p))
        out = nxt
    trig = action.get("trigger")
    if isinstance(trig, dict):
        trig = trig.get("event")
    if isinstance(trig, str) and trig in events:
        out = run(entity, events[trig], out, depth + 1)
    return merge(out)


def merge(states):
    """Identical group sets merge (the likeliest kept); the set stays small."""
    merged = {}
    for g, p in states:
        key = frozenset(g)
        if key not in merged or merged[key][1] < p:
            merged[key] = (g, p)
    return sorted(merged.values(), key=lambda gp: -gp[1])[:MAX_OUTCOMES]


def state_of(entity, groups):
    comps = dict(entity.get("components") or {})
    cg = entity.get("component_groups") or {}
    for g in groups:
        comps.update(cg.get(g, {}))
    st = {}
    for comp, field in PROPS.items():
        v = comps.get(comp)
        if isinstance(v, dict) and isinstance(v.get("value"), int):
            st[field] = v["value"]
    for comp, field in FLAGS.items():
        st[field] = 1 if comp in comps else 0
    return st


def properties_of(entity):
    """Declared entity properties (description.properties) with their defaults, as the game saves them under
    "properties": enum -> String, bool -> Byte, int -> Int. Float properties are left out (none on vanilla mobs that
    Bridger writes)."""
    out = []
    for name, decl in ((entity.get("description") or {}).get("properties") or {}).items():
        kind, default = decl.get("type"), decl.get("default")
        if kind == "enum" and isinstance(default, str):
            out.append({"name": s(name), "enum": s(default)})
        elif kind == "bool" and isinstance(default, bool):
            out.append({"name": s(name), "bool": b(1 if default else 0)})
        elif kind == "int" and isinstance(default, int):
            out.append({"name": s(name), "int": i(default)})
    return out


def health_of(entity, groups):
    """The mob's maximum health (minecraft:health "max", else "value"), from its base components and the groups of its
    likeliest spawn outcome; 0 for inanimate types. Bridger treats a type with health as a mob."""
    comps = dict(entity.get("components") or {})
    for g in groups:
        comps.update((entity.get("component_groups") or {}).get(g) or {})
    h = comps.get("minecraft:health")
    family = (comps.get("minecraft:type_family") or {}).get("family") or []
    if not isinstance(h, dict) or "inanimate" in family:  # experience orbs have health but are no mob
        return 0
    v = h.get("max", h.get("value"))
    if isinstance(v, dict):  # {"range_min": .., "range_max": ..}
        v = v.get("range_max", v.get("range_min"))
    return int(v) if isinstance(v, (int, float)) else 0


def outcomes(entity):
    events = (entity.get("events") or {})
    res = []
    if "minecraft:entity_spawned" in events:
        res += run(entity, events["minecraft:entity_spawned"], [((), 1.0)])
    if "minecraft:entity_born" in events:
        res += [(g, p * 0.01) for g, p in run(entity, events["minecraft:entity_born"], [((), 1.0)])]
    if not res:
        res = [((), 1.0)]
    merged = {}
    for g, p in res:
        key = frozenset(g)
        if key not in merged or merged[key][1] < p:
            merged[key] = (g, p)
    return sorted(merged.values(), key=lambda gp: -gp[1])[:MAX_OUTCOMES]


# ---- Choosing an outcome (mirrors bedrock_entities.cpp) -------------------------------------------------------------
_names_cache = {}


def names_of(rows):
    """The distinct group names of a mob's outcomes, in first-seen order."""
    key = id(rows)
    if key not in _names_cache:
        seen = []
        for r in rows:
            for g in r["groups"]:
                if g not in seen:
                    seen.append(g)
        if len(seen) > 255:
            sys.exit("more than 255 component groups for one mob")
        _names_cache[key] = seen
    return _names_cache[key]


def choose(table, desired):
    best, best_key = None, None
    for o in table:
        if o.get("baby", 0) != desired.get("baby", 0):
            continue
        score = sum(1 for k, v in desired.items() if k != "baby" and o.get(k, 0) == v)
        key = (score, o["p"])
        if best_key is None or key > best_key:
            best, best_key = o, key
    return best


def main():
    files = fetch_files()
    table, extra = {}, {}
    for name, text in files.items():
        doc = parse(text).get("minecraft:entity")
        if not doc:
            continue
        ident = doc.get("description", {}).get("identifier")
        if not ident:
            continue
        rows = []
        for groups, p in outcomes(doc):
            row = {"groups": list(groups), "p": max(1, int(round(p * 1_000_000)))}
            row.update(state_of(doc, groups))
            rows.append(row)
        table[ident] = rows
        extra[ident] = {"properties": properties_of(doc), "health": health_of(doc, rows[0]["groups"] if rows else [])}

    if "--check" in sys.argv:
        real = json.loads(Path(sys.argv[sys.argv.index("--check") + 1]).read_text(encoding="utf-8"))
        ok = bad = 0
        for ident, info in real.items():
            if ident not in table:
                continue
            for row in info["sets"]:
                st = row["state"]
                desired = {"baby": st.get("IsBaby", 0)}
                for tag, field in (("Variant", "variant"), ("MarkVariant", "mark_variant"), ("Color", "color")):
                    desired[field] = st.get(tag, 0)
                pick = choose(table[ident], desired)
                want = sorted({d[1:] for d in row["definitions"] if d.startswith("+") and d[1:] != ident})
                got = sorted(set(pick["groups"])) if pick else None
                if got == want:
                    ok += 1
                else:
                    bad += 1
                    print(f"  differs: {ident} {st}\n    game:    {want}\n    bridger: {got}")
        print(f"check against real worlds: {ok} match, {bad} differ")

    root = c({
        "source": s(f"Mojang/bedrock-samples@{COMMIT} ({TAG}) behavior_pack/entities"),
        # Each outcome's groups are indices into the mob's "names" (the file stays small).
        "entities": c({ident: c({
            "names": lst(STRING, [s(n) for n in names_of(rows)]),
            "outcomes": lst(COMPOUND, [
                c({k: (Tag(BARR, [names_of(rows).index(g) for g in v]) if k == "groups" else i(v))
                   for k, v in row.items() if k in ("groups", "p") or v != 0})  # absent = 0 (Bedrock's default)
                for row in rows
            ]),
            "properties": lst(COMPOUND, [c(prop) for prop in extra[ident]["properties"]]),
            **({"health": i(extra[ident]["health"])} if extra[ident]["health"] else {}),
        }) for ident, rows in sorted(table.items())}),
    })
    out = io.BytesIO()
    out.write(struct.pack("<b", COMPOUND))
    w_str(out, "")
    w_payload(out, root)
    OUT.write_bytes(out.getvalue())
    print(f"wrote {OUT} ({len(out.getvalue())} bytes, {len(table)} entities, "
          f"{sum(len(r) for r in table.values())} outcomes)")


if __name__ == "__main__":
    main()
