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
import zlib
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


# --------------------------------------------------------------------------- in-game test structures
# Fixed structures for the in-game checklist (docs/testing.md). Every state is a valid
# Java 26.3 state (checked against Mojang's generated blocks report).

def v3_schem(size, blocks, block_entities=(), entities=()):
    """blocks: {(x,y,z): state}; unspecified positions are air."""
    w, h, l = size
    palette = {"minecraft:air": 0}
    idx = [0] * (w * h * l)
    for (x, y, z), state in blocks.items():
        if state not in palette:
            palette[state] = len(palette)
        idx[index_of(x, y, z, w, l)] = palette[state]
    s = Compound({
        "Version": Int(3), "DataVersion": Int(5023),
        "Width": Short(w), "Height": Short(h), "Length": Short(l),
        "Metadata": Compound({"Name": String("Bridger in-game test"), "Author": String("bridger")}),
        "Blocks": Compound({
            "Palette": Compound({k: Int(v) for k, v in palette.items()}),
            "Data": byte_array(varints(idx)),
            "BlockEntities": List[Compound]([Compound({"Pos": IntArray(list(p)), "Id": String(i), "Data": d})
                                             for p, i, d in block_entities]),
        }),
    })
    if entities:
        s["Entities"] = List[Compound]([Compound({"Pos": List[Double](list(p)), "Id": String(i), "Data": d})
                                        for p, i, d in entities])
    return Compound({"Schematic": s})


def items(*entries):
    return List[Compound]([Compound({"Slot": Byte(slot), "id": String(item), "count": Int(count)})
                           for slot, item, count in entries])


def ingame_orientations():
    b = {}
    for x in range(12):
        for z in range(10):
            b[(x, 0, z)] = "minecraft:smooth_stone"
    facings = ["north", "east", "south", "west"]
    # Row 1: stairs, every facing, bottom (z=1) and top (z=2) halves.
    for i, f in enumerate(facings):
        b[(1 + 2 * i, 1, 1)] = f"minecraft:oak_stairs[facing={f},half=bottom,shape=straight,waterlogged=false]"
        b[(1 + 2 * i, 2, 2)] = f"minecraft:oak_stairs[facing={f},half=top,shape=straight,waterlogged=false]"
    # Inner/outer corner shapes.
    for i, shape in enumerate(["inner_left", "inner_right", "outer_left", "outer_right"]):
        b[(1 + 2 * i, 1, 4)] = f"minecraft:stone_brick_stairs[facing=north,half=bottom,shape={shape},waterlogged=false]"
    # Doors: closed left hinge, open right hinge (lower + upper halves).
    for i, (f, hinge, open_) in enumerate([("north", "left", "false"), ("east", "right", "true"),
                                           ("south", "left", "true"), ("west", "right", "false")]):
        x = 1 + 2 * i
        b[(x, 1, 6)] = f"minecraft:oak_door[facing={f},half=lower,hinge={hinge},open={open_},powered=false]"
        b[(x, 2, 6)] = f"minecraft:oak_door[facing={f},half=upper,hinge={hinge},open={open_},powered=false]"
    # Logs on every axis, trapdoors.
    for i, axis in enumerate(["x", "y", "z"]):
        b[(9, 1 + i, 1)] = f"minecraft:oak_log[axis={axis}]"
    b[(10, 1, 1)] = "minecraft:oak_trapdoor[facing=north,half=bottom,open=false,powered=false,waterlogged=false]"
    b[(10, 1, 2)] = "minecraft:oak_trapdoor[facing=east,half=top,open=true,powered=false,waterlogged=false]"
    # Rails: straight, curve, ascending, powered.
    b[(1, 1, 8)] = "minecraft:rail[shape=east_west,waterlogged=false]"
    b[(2, 1, 8)] = "minecraft:rail[shape=south_west,waterlogged=false]"
    b[(2, 1, 9)] = "minecraft:rail[shape=north_south,waterlogged=false]"
    b[(4, 1, 8)] = "minecraft:powered_rail[powered=true,shape=east_west,waterlogged=false]"
    b[(5, 1, 8)] = "minecraft:powered_rail[powered=false,shape=ascending_east,waterlogged=false]"
    b[(6, 2, 8)] = "minecraft:smooth_stone"
    # Wall torches and a wall.
    b[(8, 2, 8)] = "minecraft:wall_torch[facing=south]"
    b[(8, 1, 8)] = "minecraft:stone_bricks"
    b[(8, 2, 7)] = "minecraft:stone_bricks"
    b[(10, 1, 8)] = "minecraft:stone_brick_wall[east=none,north=none,south=none,up=true,waterlogged=false,west=none]"
    return v3_schem((12, 4, 10), b)


def ingame_waterlogged():
    b = {}
    for x in range(10):
        for z in range(5):
            b[(x, 0, z)] = "minecraft:stone"
    b[(1, 1, 1)] = "minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=true]"
    b[(2, 1, 1)] = "minecraft:oak_slab[type=bottom,waterlogged=true]"
    b[(3, 1, 1)] = "minecraft:oak_fence[east=false,north=false,south=false,waterlogged=true,west=false]"
    b[(4, 1, 1)] = "minecraft:chest[facing=south,type=single,waterlogged=true]"
    b[(5, 1, 1)] = "minecraft:lantern[hanging=false,waterlogged=true]"
    b[(6, 1, 1)] = "minecraft:glass_pane[east=false,north=false,south=false,waterlogged=true,west=false]"
    b[(7, 1, 1)] = "minecraft:oak_trapdoor[facing=north,half=bottom,open=false,powered=false,waterlogged=true]"
    b[(2, 1, 3)] = "minecraft:water[level=0]"
    b[(5, 1, 3)] = "minecraft:lava[level=0]"
    for x in range(10):  # a rim so sources stay put
        b[(x, 1, 4)] = "minecraft:glass"
    return v3_schem((10, 3, 5), b, [((4, 1, 1), "minecraft:chest", Compound({"Items": items((0, "minecraft:cod", 3))}))])


def ingame_containers():
    b = {}
    for x in range(12):
        for z in range(4):
            b[(x, 0, z)] = "minecraft:stone"
    be = []
    b[(1, 1, 1)] = "minecraft:chest[facing=south,type=single,waterlogged=false]"
    be.append(((1, 1, 1), "minecraft:chest", Compound({"Items": items((0, "minecraft:diamond", 5), (13, "minecraft:oak_log", 64),
                                                                       (26, "minecraft:diamond_sword", 1)),
                                                       "CustomName": String("Single chest")})))
    # Double chest facing north: left half at x=3 pairs with the right half at x=4 (x+1).
    b[(3, 1, 1)] = "minecraft:chest[facing=north,type=left,waterlogged=false]"
    b[(4, 1, 1)] = "minecraft:chest[facing=north,type=right,waterlogged=false]"
    be.append(((3, 1, 1), "minecraft:chest", Compound({"Items": items((0, "minecraft:emerald", 10))})))
    be.append(((4, 1, 1), "minecraft:chest", Compound({"Items": items((26, "minecraft:gold_ingot", 20))})))
    b[(6, 1, 1)] = "minecraft:trapped_chest[facing=south,type=single,waterlogged=false]"
    be.append(((6, 1, 1), "minecraft:trapped_chest", Compound({"Items": items((4, "minecraft:tnt", 2))})))
    b[(7, 1, 1)] = "minecraft:barrel[facing=up,open=false]"
    be.append(((7, 1, 1), "minecraft:barrel", Compound({"Items": items((0, "minecraft:white_wool", 16))})))
    b[(8, 1, 1)] = "minecraft:furnace[facing=south,lit=false]"
    be.append(((8, 1, 1), "minecraft:furnace", Compound({"Items": items((0, "minecraft:iron_ore", 8), (1, "minecraft:coal", 4))})))
    b[(9, 1, 1)] = "minecraft:hopper[enabled=true,facing=down]"
    be.append(((9, 1, 1), "minecraft:hopper", Compound({"Items": items((2, "minecraft:apple", 3))})))
    b[(10, 1, 1)] = "minecraft:red_shulker_box[facing=up]"
    be.append(((10, 1, 1), "minecraft:shulker_box", Compound({"Items": items((0, "minecraft:ender_pearl", 16))})))
    return v3_schem((12, 3, 4), b, be)


def ingame_signs_beds():
    b = {}
    for x in range(12):
        for z in range(6):
            b[(x, 0, z)] = "minecraft:stone"
    be = []

    def sign(lines, back=("", "", "", ""), glow=False, color="black"):
        face = lambda ls: Compound({"messages": List[String]([String(t) for t in ls]), "color": String(color),
                                    "has_glowing_text": Byte(1 if glow else 0)})
        return Compound({"front_text": face(lines), "back_text": face(back), "is_waxed": Byte(0)})

    b[(1, 1, 1)] = "minecraft:oak_sign[rotation=8,waterlogged=false]"
    be.append(((1, 1, 1), "minecraft:sign", sign(("Bridger", "front", "line 3", "line 4"), ("back", "", "", "side"))))
    b[(3, 1, 1)] = "minecraft:stone_bricks"
    b[(3, 1, 2)] = "minecraft:oak_wall_sign[facing=south,waterlogged=false]"
    be.append(((3, 1, 2), "minecraft:sign", sign(("Wall sign", "", "", ""))))
    b[(5, 1, 1)] = "minecraft:oak_sign[rotation=0,waterlogged=false]"
    be.append(((5, 1, 1), "minecraft:sign", sign(("Glowing", "text", "", ""), glow=True)))
    # Beds: foot at z, head at z+1 when facing south.
    for i, color in enumerate(["red", "white", "blue", "lime"]):
        x = 1 + 2 * i
        b[(x, 1, 3)] = f"minecraft:{color}_bed[facing=south,occupied=false,part=foot]"
        b[(x, 1, 4)] = f"minecraft:{color}_bed[facing=south,occupied=false,part=head]"
        be.append(((x, 1, 3), "minecraft:bed", Compound({})))
        be.append(((x, 1, 4), "minecraft:bed", Compound({})))
    ents = [((9.5, 1.0, 1.5), "minecraft:armor_stand", Compound({"Invisible": Byte(0)}))]
    return v3_schem((12, 3, 6), b, be, ents)


def ingame_borders():
    """40x40x40: crosses chunk (x,z) and subchunk (y) borders; centered on 0,0 it also
    spans negative coordinates. Corner pillars and a checkerboard floor show alignment."""
    n = 40
    b = {}
    for x in range(n):
        for z in range(n):
            b[(x, 0, z)] = "minecraft:white_concrete" if ((x // 4 + z // 4) % 2) else "minecraft:black_concrete"
    for x, z in [(0, 0), (n - 1, 0), (0, n - 1), (n - 1, n - 1), (15, 15), (16, 16), (31, 31), (32, 32)]:
        for y in range(1, n):
            b[(x, y, z)] = "minecraft:glowstone" if y % 16 == 0 else "minecraft:quartz_block"
    for y in range(1, n):  # a glass column every 16 blocks marks subchunk borders
        b[(20, y, 20)] = "minecraft:red_stained_glass" if y % 16 == 0 else "minecraft:glass"
    return v3_schem((n, n, n), b)


def generate_ingame():
    for name, root in [("orientations", ingame_orientations()), ("waterlogged", ingame_waterlogged()),
                       ("containers", ingame_containers()), ("signs_beds", ingame_signs_beds()),
                       ("borders", ingame_borders())]:
        write_gzip(ROOT / "ingame" / f"{name}.schem", nbt_bytes(root, "", "big"))


# --------------------------------------------------------------------------- other Java formats

def generate_structure_nbt():
    """Java structure files: the 26.3 palette spelling ({id, properties}, as in Mojang's own
    files) and the older one ({Name, Properties}); expected content is identical."""
    blocks_spec = [((0, 0, 0), 0), ((1, 0, 0), 0), ((2, 0, 0), 1), ((1, 1, 0), 2), ((2, 1, 1), 3)]
    palette = [("minecraft:stone", {}),
               ("minecraft:oak_stairs", {"facing": "east", "half": "bottom", "shape": "straight", "waterlogged": "false"}),
               ("minecraft:chest", {"facing": "north", "type": "single", "waterlogged": "true"}),
               ("minecraft:red_wool", {})]

    def make(name_key, props_key):
        pal = List[Compound]([Compound({name_key: String(n), **({props_key: Compound({k: String(v) for k, v in p.items()})} if p else {})})
                              for n, p in palette])
        blocks = []
        for pos, state in blocks_spec:
            b = Compound({"pos": List[Int](list(pos)), "state": Int(state)})
            if state == 2:
                b["nbt"] = Compound({"id": String("minecraft:chest"), "Items": items((0, "minecraft:diamond", 7))})
            blocks.append(b)
        ents = List[Compound]([Compound({"pos": List[Double]([1.5, 1.0, 1.5]), "blockPos": List[Int]([1, 1, 1]),
                                         "nbt": Compound({"id": String("minecraft:armor_stand")})})])
        return Compound({"DataVersion": Int(5023), "size": List[Int]([3, 2, 2]), "palette": pal,
                         "blocks": List[Compound](blocks), "entities": ents})

    write_gzip(ROOT / "structure" / "modern_keys.nbt", nbt_bytes(make("id", "properties"), "", "big"))
    write_gzip(ROOT / "structure" / "legacy_keys.nbt", nbt_bytes(make("Name", "Properties"), "", "big"))
    # Shipwreck-style: several palettes (the first one is used, with a warning).
    root = make("id", "properties")
    first = root["palette"]
    del root["palette"]
    root["palettes"] = List[List[Compound]]([first, first])
    write_gzip(ROOT / "structure" / "multi_palette.nbt", nbt_bytes(root, "", "big"))


def pack_litematic(indices, bits):
    """Litematica packing: entries may span two longs, LSB first."""
    total = len(indices) * bits
    longs = [0] * ((total + 63) // 64)
    for i, v in enumerate(indices):
        start = i * bits
        w, off = divmod(start, 64)
        longs[w] |= (v << off) & 0xFFFFFFFFFFFFFFFF
        if off + bits > 64:
            longs[w + 1] |= v >> (64 - off)
    return LongArray([l - (1 << 64) if l >= (1 << 63) else l for l in longs])


def generate_litematic():
    # Region A at (0,0,0) size (5,2,3); region B at Position (10,0,0) Size (-3,2,2): x 8..10.
    pal_a = ["minecraft:air", "minecraft:stone", "minecraft:oak_planks", "minecraft:glass", "minecraft:bricks",
             "minecraft:chest[facing=south,type=single,waterlogged=false]"]  # 6 entries -> 3 bits (spans longs)

    def entry(state):
        name, props = (state.split("[", 1) + [""])[:2]
        props = dict(p.split("=") for p in props.rstrip("]").split(",")) if props else {}
        c = Compound({"Name": String(name)})
        if props:
            c["Properties"] = Compound({k: String(v) for k, v in props.items()})
        return c

    sx, sy, sz = 5, 2, 3
    idx_a = []
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                idx_a.append(5 if (x, y, z) == (2, 1, 1) else (1 + (x + z) % 4 if y == 0 else 0))
    region_a = Compound({
        "Position": Compound({"x": Int(0), "y": Int(0), "z": Int(0)}),
        "Size": Compound({"x": Int(sx), "y": Int(sy), "z": Int(sz)}),
        "BlockStatePalette": List[Compound]([entry(p) for p in pal_a]),
        "BlockStates": pack_litematic(idx_a, 3),
        "TileEntities": List[Compound]([Compound({"x": Int(2), "y": Int(1), "z": Int(1), "id": String("minecraft:chest"),
                                                  "Items": items((3, "minecraft:emerald", 9))})]),
        "Entities": List[Compound]([]),
    })
    pal_b = ["minecraft:air", "minecraft:gold_block"]
    region_b = Compound({
        "Position": Compound({"x": Int(10), "y": Int(0), "z": Int(0)}),
        "Size": Compound({"x": Int(-3), "y": Int(2), "z": Int(2)}),
        "BlockStatePalette": List[Compound]([entry(p) for p in pal_b]),
        "BlockStates": pack_litematic([1] * (3 * 2 * 2), 2),
        "TileEntities": List[Compound]([]),
        "Entities": List[Compound]([]),
    })
    root = Compound({
        "MinecraftDataVersion": Int(5023), "Version": Int(7), "SubVersion": Int(1),
        "Metadata": Compound({"Name": String("Two regions"), "Author": String("bridger"), "TimeCreated": Long(1760000000000)}),
        "Regions": Compound({"A": region_a, "B": region_b}),
    })
    write_gzip(ROOT / "litematic" / "two_regions.litematic", nbt_bytes(root, "", "big"))


def generate_legacy_schematic():
    w, h, l = 4, 2, 3
    ids = [0] * (w * h * l)
    data = [0] * (w * h * l)

    def put(x, y, z, block_id, d=0):
        i = (y * l + z) * w + x
        ids[i] = block_id
        data[i] = d

    for x in range(w):
        for z in range(l):
            put(x, 0, z, 1)          # stone
    put(0, 1, 0, 53, 1)              # oak stairs facing west (legacy data 1)
    put(1, 1, 0, 35, 14)             # red wool
    put(2, 1, 0, 54, 3)              # chest facing south
    put(3, 1, 0, 44, 0)              # legacy stone slab = smooth stone slab since 1.14
    put(0, 1, 1, 31, 1)              # tall grass = short_grass since 1.20.3
    put(1, 1, 1, 26, 0)              # bed foot (red by id; blue via block entity color)
    put(3, 1, 2, 300, 0)             # id 300: needs AddBlocks; unknown -> reported
    add = [0] * ((len(ids) + 1) // 2)
    for i, v in enumerate(ids):
        hi = v >> 8
        if hi:
            add[i >> 1] |= (hi << 4) if i % 2 == 0 else hi  # even index -> high nibble
    root = Compound({
        "Width": Short(w), "Height": Short(h), "Length": Short(l), "Materials": String("Alpha"),
        "Blocks": byte_array([v & 0xFF for v in ids]), "Data": byte_array(data), "AddBlocks": byte_array(add),
        "TileEntities": List[Compound]([
            Compound({"id": String("Chest"), "x": Int(2), "y": Int(1), "z": Int(0),
                      "Items": List[Compound]([Compound({"id": Short(264), "Count": Byte(5), "Damage": Short(0), "Slot": Byte(0)}),
                                               Compound({"id": Short(35), "Count": Byte(3), "Damage": Short(11), "Slot": Byte(1)})])}),
            Compound({"id": String("minecraft:bed"), "x": Int(1), "y": Int(1), "z": Int(1), "color": Int(11)}),
        ]),
        "Entities": List[Compound]([]),
        "WEOffsetX": Int(0), "WEOffsetY": Int(-1), "WEOffsetZ": Int(0),
    })
    write_gzip(ROOT / "schematic" / "legacy_basic.schematic", nbt_bytes(root, "Schematic", "big"))


def pack_anvil(indices, bits):
    """Anvil 1.16+ packing: 64 // bits entries per long, never spanning, LSB first."""
    per = 64 // bits
    longs = [0] * ((len(indices) + per - 1) // per)
    for i, v in enumerate(indices):
        longs[i // per] |= v << ((i % per) * bits)
    return LongArray([l - (1 << 64) if l >= (1 << 63) else l for l in longs])


def anvil_state(state, spelling="Name"):
    name, props = (state.split("[", 1) + [""])[:2]
    props = dict(p.split("=") for p in props.rstrip("]").split(",")) if props else {}
    keys = ("Name", "Properties") if spelling == "Name" else ("id", "properties")
    c = Compound({keys[0]: String(name)})
    if props:
        c[keys[1]] = Compound({k: String(v) for k, v in props.items()})
    return c


def anvil_section(y, palette, indices=None, biomes=("minecraft:plains",), biome_indices=None, spelling="Name"):
    states = Compound({"palette": List[Compound]([anvil_state(p, spelling) for p in palette])})
    if len(palette) > 1:
        states["data"] = pack_anvil(indices, max(4, (len(palette) - 1).bit_length()))
    bio = Compound({"palette": List[String]([String(b) for b in biomes])})
    if len(biomes) > 1:
        bio["data"] = pack_anvil(biome_indices, (len(biomes) - 1).bit_length())
    return Compound({"Y": Byte(y), "block_states": states, "biomes": bio})


def anvil_chunk(cx, cz, sections, block_entities=(), status="minecraft:full"):
    return Compound({
        "DataVersion": Int(5023), "xPos": Int(cx), "zPos": Int(cz), "yPos": Int(-4), "Status": String(status),
        "sections": List[Compound](sections), "block_entities": List[Compound](list(block_entities)),
    })


def write_region(path, chunks):
    """chunks: {(local_x, local_z): (compression, nbt_bytes, external)}; external payloads go to c.X.Z.mcc."""
    rx, rz = (int(v) for v in path.name.split(".")[1:3])
    header = bytearray(8192)
    body = bytearray()
    for (lx, lz), (kind, raw, external) in sorted(chunks.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        if kind == 1:
            buf = io.BytesIO()
            with gzip.GzipFile(filename="", mode="wb", fileobj=buf, mtime=0) as gz:
                gz.write(raw)
            data = buf.getvalue()
        elif kind == 2:
            data = zlib.compress(raw, 6)
        else:
            data = raw
        if external:
            write_raw(path.parent / f"c.{rx * 32 + lx}.{rz * 32 + lz}.mcc", data)
            payload = struct.pack(">iB", 1, kind | 0x80)
        else:
            payload = struct.pack(">iB", len(data) + 1, kind) + data
        payload += bytes((-len(payload)) % 4096)
        offset = 2 + len(body) // 4096
        sectors = len(payload) // 4096
        struct.pack_into(">I", header, 4 * (lx + lz * 32), (offset << 8) | sectors)
        struct.pack_into(">I", header, 4096 + 4 * (lx + lz * 32), 1760000000)
        body += payload
    write_raw(path, bytes(header) + bytes(body))


def generate_java_world():
    world = ROOT / "javaworld" / "basic"
    level = Compound({"Data": Compound({
        "LevelName": String("Bridger Test World"), "DataVersion": Int(5023),
        "Version": Compound({"Name": String("26.3"), "Id": Int(5023)}),
        "spawn": Compound({"dimension": String("minecraft:overworld"), "pos": IntArray([8, 70, 8])}),
        "WorldGenSettings": Compound({"seed": Long(12345)}),
    })})
    write_gzip(world / "level.dat", nbt_bytes(level, "", "big"))

    # Chunk (0,0), zlib: stone floor section Y=-4, a mixed section Y=4 (y 64..79) with a
    # chest at (3,70,5) and two biomes (desert where x >= 8), an all-air section Y=19.
    pal = ["minecraft:air", "minecraft:stone", "minecraft:oak_planks", "minecraft:glass",
           "minecraft:chest[facing=north,type=single,waterlogged=false]"]
    idx = [0] * 4096
    for z in range(16):
        for x in range(16):
            idx[(0 << 8) | (z << 4) | x] = 2  # y 64: planks
            idx[(1 << 8) | (z << 4) | x] = 3 if x == 0 else 0  # y 65: glass wall at x=0
    idx[(6 << 8) | (5 << 4) | 3] = 4  # chest at (3,70,5)
    bidx = [1 if (i % 4) >= 2 else 0 for i in range(64)]  # x quarter >= 2 -> desert
    chest = Compound({"id": String("minecraft:chest"), "x": Int(3), "y": Int(70), "z": Int(5),
                      "keepPacked": Byte(0), "Items": items((0, "minecraft:diamond", 3))})
    c00 = anvil_chunk(0, 0, [
        anvil_section(-4, ["minecraft:stone"]),
        anvil_section(4, pal, idx, ("minecraft:plains", "minecraft:desert"), bidx),
        anvil_section(19, ["minecraft:air"]),
    ], [chest])

    # Chunk (1,0), gzip, {id, properties} spelling: a waterlogged stair at (16+2, 0, 3).
    pal2 = ["minecraft:air", "minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=true]"]
    idx2 = [0] * 4096
    idx2[(0 << 8) | (3 << 4) | 2] = 1
    c10 = anvil_chunk(1, 0, [anvil_section(0, pal2, idx2, spelling="id")])

    # Chunk (2,0), uncompressed; chunk (0,1) stored externally; chunk (1,1) not fully generated.
    c20 = anvil_chunk(2, 0, [anvil_section(0, ["minecraft:gold_block"])])
    c01 = anvil_chunk(0, 1, [anvil_section(1, ["minecraft:diamond_block"])])
    c11 = anvil_chunk(1, 1, [anvil_section(0, ["minecraft:stone"])], status="minecraft:features")
    write_region(world / "region" / "r.0.0.mca", {
        (0, 0): (2, nbt_bytes(c00, "", "big"), False),
        (1, 0): (1, nbt_bytes(c10, "", "big"), False),
        (2, 0): (3, nbt_bytes(c20, "", "big"), False),
        (0, 1): (2, nbt_bytes(c01, "", "big"), True),
        (1, 1): (2, nbt_bytes(c11, "", "big"), False),
    })
    # Negative coordinates: chunk (-1,0) is local (31,0) of region (-1,0).
    cm1 = anvil_chunk(-1, 0, [anvil_section(-1, ["minecraft:emerald_block"])])
    write_region(world / "region" / "r.-1.0.mca", {(31, 0): (2, nbt_bytes(cm1, "", "big"), False)})
    # A Nether folder with an empty region file: detected and reported, not converted.
    write_raw(world / "DIM-1" / "region" / "r.0.0.mca", b"")

    # Java 26.1+ folder layout: dimensions/minecraft/overworld/region and the seed in
    # data/minecraft/world_gen_settings.dat (saved-data wrapper {DataVersion, data}).
    modern = ROOT / "javaworld" / "modern_layout"
    write_gzip(modern / "level.dat", nbt_bytes(Compound({"Data": Compound({
        "LevelName": String("Modern Layout"), "DataVersion": Int(5023),
        "spawn": Compound({"dimension": String("minecraft:overworld"), "pos": IntArray([0, 64, 0])}),
    })}), "", "big"))
    write_gzip(modern / "data" / "minecraft" / "world_gen_settings.dat", nbt_bytes(Compound({
        "DataVersion": Int(5023),
        "data": Compound({"seed": Long(-987654321), "generate_structures": Byte(1), "bonus_chest": Byte(0)}),
    }), "", "big"))
    cm = anvil_chunk(3, -2, [anvil_section(0, ["minecraft:lapis_block"])])
    write_region(modern / "dimensions" / "minecraft" / "overworld" / "region" / "r.0.-1.mca",
                 {(3, 30): (2, nbt_bytes(cm, "", "big"), False)})
    write_raw(modern / "dimensions" / "minecraft" / "the_end" / "region" / "r.0.0.mca", b"")

    # Old (pre-1.18) chunk layout: rejected with a clear message.
    old = Compound({"DataVersion": Int(2586), "Level": Compound({"xPos": Int(0), "zPos": Int(0)})})
    write_region(ROOT / "javaworld" / "old_layout" / "region" / "r.0.0.mca", {(0, 0): (2, nbt_bytes(old, "", "big"), False)})
    write_gzip(ROOT / "javaworld" / "old_layout" / "level.dat",
               nbt_bytes(Compound({"Data": Compound({"LevelName": String("Old"), "DataVersion": Int(2586)})}), "", "big"))


if __name__ == "__main__":
    generate_nbt()
    generate_schem()
    generate_ingame()
    generate_structure_nbt()
    generate_litematic()
    generate_legacy_schematic()
    generate_java_world()
    print("test data written")
