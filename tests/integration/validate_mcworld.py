#!/usr/bin/env python3
"""Independent validation of a .mcworld written by Bridger (docs/testing.md).

Uses implementations that share no code with Bridger: amulet-leveldb (Mojang
LevelDB format; used only as a test oracle) and nbtlib (NBT). Subchunks are
decoded here from the format description in docs/formats/bedrock-world.md, and
every palette entry is checked against the official Bedrock block palette
(exact name, state value types and block version).

    python -m pip install nbtlib==2.0.4 amulet-leveldb==1.0.7
    python tests/integration/validate_mcworld.py OUT.mcworld --palette tools/mapgen/sources/cache/block_palette.26_50.nbt \
        [--report OUT.json]

Exit code 0 = valid. Prints a summary.
"""

import argparse
import gzip
import io
import json
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

import leveldb  # amulet-leveldb
import nbtlib
from nbtlib.tag import Byte, Int, Short, String

ALLOWED_BITS = {1, 2, 3, 4, 5, 6, 8, 16}
LIQUID_LAYER_OK = {"minecraft:air", "minecraft:water", "minecraft:flowing_water", "minecraft:lava",
                   "minecraft:flowing_lava"}


def fail(msg):
    print("INVALID:", msg)
    sys.exit(1)


def typed_key(name, states):
    parts = []
    for k in sorted(states):
        v = states[k]
        t = "b" if isinstance(v, Byte) else "i" if isinstance(v, (Int, Short)) else "s" if isinstance(v, String) else "?"
        parts.append(f"{k}={t}:{v}")
    return name + "[" + ",".join(parts) + "]"


def load_palette(path):
    root = nbtlib.File.parse(io.BytesIO(gzip.decompress(Path(path).read_bytes())), byteorder="big")
    keys, version = set(), None
    for b in root["blocks"]:
        keys.add(typed_key(str(b["name"]), b["states"]))
        version = int(b["version"])
    return keys, version


def read_nbt_le(buf):
    """Parses one little-endian named root at the stream position; returns the compound."""
    return nbtlib.File.parse(buf, byteorder="little")


def decode_subchunk(data, palette_keys, block_version):
    buf = io.BytesIO(data)
    version, layers = struct.unpack("<BB", buf.read(2))
    if version not in (8, 9):
        fail(f"subchunk version {version}")
    y = struct.unpack("<b", buf.read(1))[0] if version == 9 else None
    non_air = 0
    for layer in range(layers):
        header = buf.read(1)[0]
        if header & 1:
            fail("runtime palette flag in stored subchunk")
        bits = header >> 1
        if bits not in ALLOWED_BITS:
            fail(f"bits per block {bits}")
        per_word = 32 // bits
        words = -(-4096 // per_word)
        packed = struct.unpack(f"<{words}I", buf.read(4 * words))
        indices = []
        for w in packed:
            for k in range(per_word):
                if len(indices) == 4096:
                    break
                indices.append((w >> (k * bits)) & ((1 << bits) - 1))
        (count,) = struct.unpack("<i", buf.read(4))
        names = []
        for _ in range(count):
            entry = read_nbt_le(buf)
            name = str(entry["name"])
            if int(entry["version"]) != block_version:
                fail(f"block version {int(entry['version'])} != {block_version}")
            if typed_key(name, entry["states"]) not in palette_keys:
                fail(f"palette entry not in the official Bedrock palette: {typed_key(name, entry['states'])}")
            names.append(name)
        if max(indices) >= count:
            fail("index outside palette")
        if layer > 0:
            # Bridger only writes liquids into layer 2 (waterlogging); anything else is a bug.
            for n in names:
                if n not in LIQUID_LAYER_OK:
                    fail(f"layer {layer} contains {n}; only air and liquids are expected")
        if layer == 0:
            non_air = sum(1 for i in indices if names[i] != "minecraft:air")
    if buf.read():
        fail("trailing bytes after subchunk")
    return y, non_air


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mcworld")
    ap.add_argument("--palette", required=True)
    ap.add_argument("--report")
    ap.add_argument("--generator", type=int, default=2, help="expected Generator (2 flat, 1 infinite for converted worlds)")
    ap.add_argument("--chunk-version", type=int, default=42, help="expected chunk version byte (40, 41 or 42 by target version)")
    args = ap.parse_args()

    palette_keys, block_version = load_palette(args.palette)
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(args.mcworld) as z:
            names = set(z.namelist())
            for required in ("level.dat", "levelname.txt", "db/CURRENT"):
                if required not in names:
                    fail(f"missing {required}")
            z.extractall(tmp)

        raw = Path(tmp, "level.dat").read_bytes()
        storage, length = struct.unpack("<ii", raw[:8])
        if length != len(raw) - 8:
            fail("level.dat length header")
        level = read_nbt_le(io.BytesIO(raw[8:]))
        if storage != 10 or int(level["StorageVersion"]) != 10:
            fail("StorageVersion")
        if int(level["Generator"]) != args.generator:
            fail(f"Generator is {int(level['Generator'])}, expected {args.generator}")
        json.loads(str(level["FlatWorldLayers"]))

        db = leveldb.LevelDB(str(Path(tmp, "db")), create_if_missing=False)
        chunks = subchunks = block_entities = 0
        per_dim = {0: 0, 1: 0, 2: 0}
        # Subchunk range per dimension: Overworld -4..19, Nether 0..7, End 0..15 (minecraft.wiki "Bedrock Edition level
        # format"; Chunker Dimension).
        ranges = {0: range(-4, 20), 1: range(0, 8), 2: range(0, 16)}
        blocks_by_y = {}
        actors, digp = {}, {}
        for key, value in db.iterate():
            # Actors (1.18.30+): "actorprefix" + 8-byte key -> NBT; "digp" + chunk key -> the chunk's actor keys.
            if key.startswith(b"actorprefix") and len(key) == 19:
                actor = read_nbt_le(io.BytesIO(value))
                if "identifier" not in actor or len(actor.get("Pos", [])) != 3 or "UniqueID" not in actor:
                    fail("actor without identifier, Pos or UniqueID")
                actors[key[11:]] = actor
                continue
            if key.startswith(b"digp") and len(key) in (12, 16):
                if len(value) % 8:
                    fail("digp value is not a list of 8-byte keys")
                digp[key[4:]] = [value[i:i + 8] for i in range(0, len(value), 8)]
                continue
            # Chunk keys: x, z (8 bytes), a dimension (4 bytes) outside the Overworld, the tag, a subchunk index.
            if len(key) in (13, 14):
                dim = struct.unpack("<i", key[8:12])[0]
                tag, rest = key[12], key[13:]
            elif len(key) in (9, 10):
                dim, tag, rest = 0, key[8], key[9:]
            else:
                continue
            if dim not in ranges:
                fail(f"unknown dimension {dim}")
            if tag == 0x2F and len(rest) == 1:
                y, non_air = decode_subchunk(value, palette_keys, block_version)
                if y != struct.unpack("<b", rest)[0]:
                    fail("subchunk y index does not match key")
                if y not in ranges[dim]:
                    fail(f"subchunk {y} outside dimension {dim}")
                if dim == 0:
                    blocks_by_y[y] = blocks_by_y.get(y, 0) + non_air
                subchunks += 1
            elif rest:
                continue
            elif tag == 0x2C:
                if value != bytes([args.chunk_version]):
                    fail(f"chunk version {value!r}")
                chunks += 1
                per_dim[dim] += 1
            elif tag == 0x36:
                if struct.unpack("<i", value)[0] != 2:
                    fail("FinalizedState")
            elif tag == 0x31:
                cx, cz = struct.unpack("<ii", key[:8])
                buf = io.BytesIO(value)
                while buf.tell() < len(value):
                    be = read_nbt_le(buf)
                    x, z = int(be["x"]), int(be["z"])
                    if (x >> 4, z >> 4) != (cx, cz):
                        fail("block entity outside its chunk")
                    if "id" not in be:
                        fail("block entity without id")
                    block_entities += 1
            elif tag == 0x2B:
                if len(value) < 512:
                    fail("Data3D too short")
        db.close()
        for chunk, keys in digp.items():
            cx, cz = struct.unpack("<ii", chunk[:8])
            for k in keys:
                if k not in actors:
                    fail("digp lists an actor that does not exist")
                pos = actors[k]["Pos"]
                if (int(float(pos[0])) // 16, int(float(pos[2])) // 16) != (cx, cz):
                    fail("actor listed in the digp entry of another chunk")
        if len(actors) != sum(len(v) for v in digp.values()):
            fail("actors not listed in any digp entry")

    dims = "".join(f", {n} {name} chunks" for d, name in ((1, "Nether"), (2, "End")) if (n := per_dim[d]))
    dims += f", {len(actors)} actors" if actors else ""
    print(f"valid: {chunks} chunks{dims}, {subchunks} subchunks, {block_entities} block entities, "
          f"level '{level['LevelName']}'")
    if args.report:
        report = json.loads(Path(args.report).read_text(encoding="utf-8"))
        if report["chunks"] != chunks or report["subchunks"] != subchunks or report["block_entities"] != block_entities:
            fail(f"counts differ from Bridger's report: {report}")
        print("counts match Bridger's report")


if __name__ == "__main__":
    main()
