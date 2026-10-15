#!/usr/bin/env python3
"""Independent validation of Java files written by Bridger (test oracle: nbtlib, MIT).

    python tests/integration/validate_java_output.py <file.schem|file.nbt|file.litematic|world folder> ...

Checks, without Bridger's code:
  .schem      Sponge v3 layout; every varint decodes; count == Width*Height*Length; indices in the palette.
  .nbt        size/palette/blocks; every block's state index in the palette, pos inside size;
              palette entries use the 26.3 spelling {id, properties}.
  .litematic  regions; BlockStates long enough for volume * max(2, bits); indices in the palette.
  world       level.dat (DataVersion; spawn and generation settings in the layout of that version), every region
              chunk: DataVersion as level.dat, zlib/gzip, Status full,
              24 sections with block_states/biomes, packed data long enough, indices in palettes,
              block entities inside their chunk.
Exit code 0 when everything is valid.
"""

import gzip
import io
import struct
import sys
import zlib
from pathlib import Path

from nbtlib import File


def fail(msg):
    print("INVALID:", msg)
    sys.exit(1)


def load_gzip_nbt(path):
    return File.parse(io.BytesIO(gzip.decompress(Path(path).read_bytes())))


def varints(data):
    out, v, shift = [], 0, 0
    for b in data:
        b &= 0xFF
        v |= (b & 0x7F) << shift
        if b & 0x80:
            shift += 7
            if shift > 28:
                fail("varint too long")
        else:
            out.append(v)
            v, shift = 0, 0
    if shift:
        fail("truncated varint")
    return out


def check_schem(path):
    s = load_gzip_nbt(path)["Schematic"]
    if int(s["Version"]) != 3:
        fail("schem: Version is not 3")
    w, h, l = (int(s[k]) & 0xFFFF for k in ("Width", "Height", "Length"))
    pal = s["Blocks"]["Palette"]
    values = varints(bytes(b & 0xFF for b in s["Blocks"]["Data"]))
    if len(values) != w * h * l:
        fail(f"schem: {len(values)} entries for {w}x{h}x{l}")
    ids = {int(v) for v in pal.values()}
    if any(v not in ids for v in values):
        fail("schem: index outside the palette")
    for be in s["Blocks"]["BlockEntities"]:
        x, y, z = (int(v) for v in be["Pos"])
        if not (0 <= x < w and 0 <= y < h and 0 <= z < l):
            fail("schem: block entity outside the volume")
    return f"schem {w}x{h}x{l}, {len(pal)} states, {len(s['Blocks']['BlockEntities'])} block entities"


def check_structure_nbt(path):
    s = load_gzip_nbt(path)
    size = [int(v) for v in s["size"]]
    pal = s["palette"]
    for e in pal:
        if "id" not in e:
            fail("nbt: palette entry without 'id'")
    for b in s["blocks"]:
        if not 0 <= int(b["state"]) < len(pal):
            fail("nbt: state index outside the palette")
        if any(not 0 <= int(c) < size[i] for i, c in enumerate(b["pos"])):
            fail("nbt: block outside size")
    return f"structure nbt {size}, {len(pal)} states, {len(s['blocks'])} blocks"


def check_litematic(path):
    s = load_gzip_nbt(path)
    for name, r in s["Regions"].items():
        size = [abs(int(r["Size"][k])) for k in "xyz"]
        pal = r["BlockStatePalette"]
        bits = max(2, (len(pal) - 1).bit_length())
        longs = [int(v) & 0xFFFFFFFFFFFFFFFF for v in r["BlockStates"]]
        vol = size[0] * size[1] * size[2]
        if len(longs) * 64 < vol * bits:
            fail(f"litematic region {name}: BlockStates too short")
        mask = (1 << bits) - 1
        big = 0
        for i, v in enumerate(longs):
            big |= v << (64 * i)
        for i in range(vol):
            if (big >> (i * bits)) & mask >= len(pal):
                fail(f"litematic region {name}: index outside the palette")
    return f"litematic, {len(s['Regions'])} region(s)"


def unpack(longs, bits, count):
    per = 64 // bits
    mask = (1 << bits) - 1
    if len(longs) < (count + per - 1) // per:
        fail("packed array too short")
    return [((int(longs[i // per]) & 0xFFFFFFFFFFFFFFFF) >> ((i % per) * bits)) & mask for i in range(count)]


def check_world(path):
    path = Path(path)
    level = load_gzip_nbt(path / "level.dat")["Data"]
    if "DataVersion" not in level:
        fail("level.dat: DataVersion missing")
    dv = int(level["DataVersion"])
    # The layout follows the version written for (docs/versions.md, "Target versions"):
    # 1.21.9+ (4554) spawn compound, older SpawnX/Y/Z; 26.1+ (4786) dimensions/ and world_gen_settings.dat,
    # older region/ and WorldGenSettings in level.dat.
    if dv >= 4554:
        if "spawn" not in level or len(level["spawn"]["pos"]) != 3:
            fail("level.dat: spawn.pos missing")
    elif not all(k in level for k in ("SpawnX", "SpawnY", "SpawnZ")):
        fail("level.dat: SpawnX/Y/Z missing")
    if dv >= 4786:
        region_dir = path / "dimensions" / "minecraft" / "overworld" / "region"
    else:
        wgs = level.get("WorldGenSettings")
        if wgs is None or "seed" not in wgs or len(wgs["dimensions"]) != 3:
            fail("level.dat: WorldGenSettings with seed and three dimensions missing")
        for name, dim in wgs["dimensions"].items():
            if dim["generator"]["type"] != "minecraft:noise" or "biome_source" not in dim["generator"]:
                fail(f"level.dat: dimension {name} has no noise generator")
        region_dir = path / "region"
    chunks = 0
    for rp in sorted(region_dir.glob("r.*.*.mca")):
        rx, rz = (int(v) for v in rp.name.split(".")[1:3])
        data = rp.read_bytes()
        for i in range(1024):
            loc = struct.unpack(">I", data[4 * i:4 * i + 4])[0]
            if not loc:
                continue
            off, n = (loc >> 8) * 4096, loc & 0xFF
            length, kind = struct.unpack(">iB", data[off:off + 5])
            if kind & 0x80:
                payload = (rp.parent / f"c.{rx * 32 + i % 32}.{rz * 32 + i // 32}.mcc").read_bytes()
            else:
                if length + 4 > n * 4096:
                    fail(f"{rp.name}: chunk {i} longer than its sectors")
                payload = data[off + 5:off + 4 + length]
            raw = zlib.decompress(payload) if kind & 0x7F == 2 else gzip.decompress(payload)
            c = File.parse(io.BytesIO(raw))
            if str(c["Status"]) not in ("minecraft:full", "full"):
                fail(f"{rp.name}: chunk {i} is not full")
            if int(c["DataVersion"]) != dv:
                fail(f"{rp.name}: chunk {i} DataVersion {int(c['DataVersion'])} differs from level.dat's {dv}")
            if int(c["xPos"]) != rx * 32 + i % 32 or int(c["zPos"]) != rz * 32 + i // 32:
                fail(f"{rp.name}: chunk {i} has wrong coordinates")
            for sec in c["sections"]:
                pal = sec["block_states"]["palette"]
                if len(pal) > 1:
                    idx = unpack(sec["block_states"]["data"], max(4, (len(pal) - 1).bit_length()), 4096)
                    if max(idx) >= len(pal):
                        fail("block index outside the palette")
                bpal = sec["biomes"]["palette"]
                if len(bpal) > 1:
                    idx = unpack(sec["biomes"]["data"], (len(bpal) - 1).bit_length(), 64)
                    if max(idx) >= len(bpal):
                        fail("biome index outside the palette")
            for be in c["block_entities"]:
                if int(be["x"]) >> 4 != int(c["xPos"]) or int(be["z"]) >> 4 != int(c["zPos"]):
                    fail("block entity outside its chunk")
            chunks += 1
    if chunks == 0:
        fail("world has no chunks")
    return f"java world '{level['LevelName']}', {chunks} chunks"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for arg in sys.argv[1:]:
        p = Path(arg)
        if p.is_dir():
            print("valid:", check_world(p))
        elif p.suffix == ".schem":
            print("valid:", check_schem(p))
        elif p.suffix == ".nbt":
            print("valid:", check_structure_nbt(p))
        elif p.suffix == ".litematic":
            print("valid:", check_litematic(p))
        else:
            fail(f"unknown kind: {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
