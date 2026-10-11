#!/usr/bin/env python3
"""Generates Bridger's committed test data with an independent NBT implementation.

The files are written by nbtlib (MIT, https://github.com/vberlier/nbtlib), not by
Bridger, so reading them tests Bridger against a second implementation. Expected
results ("golden" JSON) are computed here, from the same definitions, without
Bridger's code.

    python -m pip install nbtlib==2.0.4
    python tests/data/generate_test_data.py

Outputs (all deterministic; commit them):
    tests/data/nbt/*.nbt              NBT reference files (byte-exact round-trip tests)
    tests/data/schem/*.schem          Sponge schematics v1/v2/v3 (+ malformed ones)
    tests/golden/schem/*.json         expected decoded content
"""

import gzip
import io
import json
import struct
from pathlib import Path

from nbtlib import File
from nbtlib.tag import (Byte, ByteArray, Compound, Double, Float, Int, IntArray, List, Long, LongArray, Short,
                        String)

ROOT = Path(__file__).resolve().parent
GOLDEN = ROOT.parent / "golden"


# --------------------------------------------------------------------------- helpers

def varints(values):
    out = bytearray()
    for v in values:
        while True:
            b = v & 0x7F
            v >>= 7
            if v:
                out.append(b | 0x80)
            else:
                out.append(b)
                break
    return out


def byte_array(data):
    return ByteArray([b - 256 if b > 127 else b for b in data])


def nbt_bytes(root, root_name, byteorder):
    buf = io.BytesIO()
    File(root, root_name=root_name).write(buf, byteorder=byteorder)
    return buf.getvalue()


def write_gzip(path, raw):
    # mtime=0 and no filename keep the output byte-identical across runs.
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0) as gz:
            gz.write(raw)


def write_raw(path, raw):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)


def canonical(state):
    """Canonical block state text: properties sorted by key (Bridger's IR form)."""
    if "[" not in state:
        return state if ":" in state else "minecraft:" + state
    name, props = state[:-1].split("[", 1)
    pairs = sorted(p.split("=", 1) for p in props.split(","))
    return name + "[" + ",".join(f"{k}={v}" for k, v in pairs) + "]"


def index_of(x, y, z, w, l):
    return x + z * w + y * w * l


# --------------------------------------------------------------------------- NBT reference files

def all_types():
    return Compound({
        "byte": Byte(-7),
        "short": Short(-12345),
        "int": Int(123456789),
        "long": Long(-9_000_000_000_000_000_000),
        "float": Float(0.5),
        "double": Double(-1234.5678),
        "byte_array": byte_array([0, 1, 127, 128, 255]),
        "string": String("hello world"),
        "string_unicode": String("جسر éè"),  # Arabic + Latin-1, same in UTF-8 and MUTF-8
        "empty_string": String(""),
        "int_array": IntArray([0, -1, 2147483647, -2147483648]),
        "long_array": LongArray([0, -1, 9223372036854775807, -9223372036854775808]),
        "list_of_ints": List[Int]([1, 2, 3]),
        "list_of_strings": List[String](["a", "bc", ""]),
        "list_of_compounds": List[Compound]([Compound({"id": String("minecraft:diamond"), "count": Byte(3)}),
                                             Compound({"id": String("minecraft:stick"), "count": Byte(64)})]),
        "list_of_lists": List[List[Short]]([List[Short]([1, 2]), List[Short]([])]),
        "nested": Compound({"a": Compound({"b": Compound({"c": Compound({"d": Int(4)})})})}),
        "z_last": Byte(1),  # order must be preserved (not sorted)
        "a_after_z": Byte(2),
    })


def generate_nbt():
    root = all_types()
    write_raw(ROOT / "nbt" / "all_types_be.nbt", nbt_bytes(root, "bridger test", "big"))
    write_raw(ROOT / "nbt" / "all_types_le.nbt", nbt_bytes(root, "bridger test", "little"))
    write_gzip(ROOT / "nbt" / "all_types_be.nbt.gz", nbt_bytes(root, "bridger test", "big"))


# --------------------------------------------------------------------------- schematics

BASIC_W, BASIC_H, BASIC_L = 5, 4, 3
# Palette strings deliberately list properties in non-sorted order.
BASIC_PALETTE = {
    "minecraft:air": 0,
    "minecraft:stone": 1,
    "minecraft:oak_stairs[waterlogged=false,half=bottom,facing=east,shape=straight]": 2,
    "minecraft:chest[waterlogged=true,facing=north,type=single]": 3,
    "minecraft:water[level=0]": 4,
}
CHEST_POS = (1, 1, 1)
STAIRS_POS = (2, 1, 1)
WATER_POS = (4, 3, 2)


def basic_block(x, y, z):
    if (x, y, z) == CHEST_POS:
        return 3
    if (x, y, z) == STAIRS_POS:
        return 2
    if (x, y, z) == WATER_POS:
        return 4
    return 1 if y == 0 else 0


def basic_indices():
    out = [0] * (BASIC_W * BASIC_H * BASIC_L)
    for y in range(BASIC_H):
        for z in range(BASIC_L):
            for x in range(BASIC_W):
                out[index_of(x, y, z, BASIC_W, BASIC_L)] = basic_block(x, y, z)
    return out


def chest_items():
    return List[Compound]([Compound({"Slot": Byte(0), "id": String("minecraft:diamond"), "count": Int(3)}),
                           Compound({"Slot": Byte(5), "id": String("minecraft:oak_log"), "count": Int(64)})])


def basic_metadata(version):
    m = Compound({"Name": String("Test House"), "Author": String("bridger"), "Date": Long(1760000000000),
                  "RequiredMods": List[String](["examplemod"])})
    if version == 3:
        m["WorldEdit"] = Compound({"Origin": IntArray([10, 64, -5]), "Version": String("test")})
    else:
        m["WEOffsetX"] = Int(-1)
        m["WEOffsetY"] = Int(0)
        m["WEOffsetZ"] = Int(2)
    return m


def biome_of(x):
    return "minecraft:desert" if x >= 3 else "minecraft:plains"


def basic_schem(version):
    palette = Compound({k: Int(v) for k, v in BASIC_PALETTE.items()})
    data = byte_array(varints(basic_indices()))
    chest_pos = IntArray(list(CHEST_POS))
    s = Compound({
        "Version": Int(version),
        "Width": Short(BASIC_W), "Height": Short(BASIC_H), "Length": Short(BASIC_L),
        "Offset": IntArray([1, -2, 3]),
        "Metadata": basic_metadata(version),
    })
    if version >= 2:
        s["DataVersion"] = Int(4556)
    if version == 3:
        s["Blocks"] = Compound({
            "Palette": palette, "Data": data,
            "BlockEntities": List[Compound]([Compound({
                "Pos": chest_pos, "Id": String("minecraft:chest"),
                "Data": Compound({"Items": chest_items(), "CustomName": String('{"text":"Loot"}')})})]),
        })
        bpal = {"minecraft:plains": 0, "minecraft:desert": 1}
        bidx = [0] * (BASIC_W * BASIC_H * BASIC_L)
        for y in range(BASIC_H):
            for z in range(BASIC_L):
                for x in range(BASIC_W):
                    bidx[index_of(x, y, z, BASIC_W, BASIC_L)] = bpal[biome_of(x)]
        s["Biomes"] = Compound({"Palette": Compound({k: Int(v) for k, v in bpal.items()}),
                                "Data": byte_array(varints(bidx))})
        s["Entities"] = List[Compound]([Compound({
            "Pos": List[Double]([2.5, 1.0, 1.5]), "Id": String("minecraft:armor_stand"),
            "Data": Compound({"Invisible": Byte(1)})})])
        return Compound({"Schematic": s}), ""
    s["Palette"] = palette
    s["PaletteMax"] = Int(len(BASIC_PALETTE))
    s["BlockData"] = data
    if version == 2:
        # Flattened block entity fields, as WorldEdit writes them.
        s["BlockEntities"] = List[Compound]([Compound({
            "Pos": chest_pos, "Id": String("minecraft:chest"),
            "Items": chest_items(), "CustomName": String('{"text":"Loot"}')})])
        bpal = {"minecraft:plains": 0, "minecraft:desert": 1}
        s["BiomePalette"] = Compound({k: Int(v) for k, v in bpal.items()})
        s["BiomePaletteMax"] = Int(2)
        s["BiomeData"] = byte_array(varints([bpal[biome_of(x)] for z in range(BASIC_L) for x in range(BASIC_W)]))
        s["Entities"] = List[Compound]([Compound({
            "Pos": List[Double]([2.5, 1.0, 1.5]), "Id": String("minecraft:armor_stand"), "Invisible": Byte(1)})])
    else:
        s["TileEntities"] = List[Compound]([Compound({
            "ContentVersion": Int(1), "Pos": chest_pos, "Id": String("minecraft:chest"),
            "Items": chest_items(), "CustomName": String('{"text":"Loot"}')})])
    return s, "Schematic"


def basic_golden(version):
    inv = {v: canonical(k) for k, v in BASIC_PALETTE.items()}
    blocks = {}
    for y in range(BASIC_H):
        for z in range(BASIC_L):
            for x in range(BASIC_W):
                name = inv[basic_block(x, y, z)]
                if name != "minecraft:air":
                    blocks[f"{x},{y},{z}"] = name
    g = {
        "version": version,
        "data_version": 4556 if version >= 2 else None,
        "size": [BASIC_W, BASIC_H, BASIC_L],
        "offset": [1, -2, 3],
        "palette_size": len(BASIC_PALETTE),
        "non_air": blocks,
        "block_entities": [{"pos": list(CHEST_POS), "id": "minecraft:chest", "keys": ["CustomName", "Items"]}],
        "entities": [] if version == 1 else [{"pos": [2.5, 1.0, 1.5], "id": "minecraft:armor_stand",
                                               "keys": ["Invisible"]}],
        "metadata": {"name": "Test House", "author": "bridger", "date_ms": 1760000000000,
                     "required_mods": ["examplemod"]},
        "biomes": None if version == 1 else {f"{x},0,0": biome_of(x) for x in range(BASIC_W)},
        "warnings": 0,
    }
    return g


BORDER_W, BORDER_H, BORDER_L, BORDER_PALETTE = 33, 20, 18, 300


def border_index(x, y, z):
    return (x * 7 + y * 13 + z * 31) % BORDER_PALETTE


def border_schem():
    idx = [0] * (BORDER_W * BORDER_H * BORDER_L)
    for y in range(BORDER_H):
        for z in range(BORDER_L):
            for x in range(BORDER_W):
                idx[index_of(x, y, z, BORDER_W, BORDER_L)] = border_index(x, y, z)
    names = [f"bridger:test_{i}" for i in range(BORDER_PALETTE)]
    s = Compound({
        "Version": Int(3), "DataVersion": Int(4556),
        "Width": Short(BORDER_W), "Height": Short(BORDER_H), "Length": Short(BORDER_L),
        "Blocks": Compound({"Palette": Compound({n: Int(i) for i, n in enumerate(names)}),
                            "Data": byte_array(varints(idx))}),
    })
    golden = {"version": 3, "size": [BORDER_W, BORDER_H, BORDER_L], "palette": names,
              "formula": "(x*7 + y*13 + z*31) % 300"}
    return Compound({"Schematic": s}), golden


def malformed():
    """(name, root, root_name, expected Bridger ErrorCode name)."""
    cases = []
    root, name = basic_schem(3)
    bad = root["Schematic"]["Blocks"]
    bad["Data"] = byte_array(varints([99] + basic_indices()[1:]))
    cases.append(("v3_bad_palette_index", root, name, "Malformed"))

    root, name = basic_schem(3)
    root["Schematic"]["Blocks"]["Data"] = byte_array(varints(basic_indices()[:-1]))
    cases.append(("v3_short_data", root, name, "Malformed"))

    root, name = basic_schem(3)
    root["Schematic"]["Blocks"]["Data"] = byte_array(varints(basic_indices() + [0]))
    cases.append(("v3_long_data", root, name, "Malformed"))

    root, name = basic_schem(3)
    root["Schematic"]["Version"] = Int(4)
    cases.append(("v4_unsupported", root, name, "UnsupportedVersion"))

    root, name = basic_schem(2)
    del root["Palette"]
    cases.append(("v2_no_palette", root, name, "UnsupportedFormat"))

    root, name = basic_schem(3)
    root["Schematic"]["Blocks"]["BlockEntities"][0]["Pos"] = IntArray([1, 1])
    cases.append(("v3_bad_block_entity_pos", root, name, "Malformed"))
    return cases


def generate_schem():
    goldens = {}
    for version in (1, 2, 3):
        root, root_name = basic_schem(version)
        write_gzip(ROOT / "schem" / f"v{version}_basic.schem", nbt_bytes(root, root_name, "big"))
        goldens[f"v{version}_basic"] = basic_golden(version)

    root, golden = border_schem()
    write_gzip(ROOT / "schem" / "v3_borders.schem", nbt_bytes(root, "", "big"))
    goldens["v3_borders"] = golden

    errors = {}
    for name, root, root_name, code in malformed():
        write_gzip(ROOT / "schem" / "malformed" / f"{name}.schem", nbt_bytes(root, root_name, "big"))
        errors[name] = code
    goldens["malformed"] = errors

    (GOLDEN / "schem").mkdir(parents=True, exist_ok=True)
    for name, g in goldens.items():
        (GOLDEN / "schem" / f"{name}.json").write_text(json.dumps(g, indent=1, sort_keys=True) + "\n",
                                                      encoding="utf-8", newline="\n")


if __name__ == "__main__":
    generate_nbt()
    generate_schem()
    print("test data written")
