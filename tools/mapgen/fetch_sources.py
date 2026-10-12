#!/usr/bin/env python3
"""Fetches the mapping inputs that are not git submodules, verifying pinned hashes.

Inputs (docs/adr/0007-mapping-data-sources.md):
  1. Java block states: generated locally by Mojang's official data generator from the
     26.3 server jar (SHA-1 from Mojang's launcher manifest). Requires Java 25+.
     The jar and its reports are never committed.
  2. Bedrock block palette (names, typed states, block version) for 26.50, from the
     GeyserMC/Geyser repository (MIT) at a pinned commit.
GeyserMC/mappings (Java <-> Bedrock mapping) is the git submodule sources/geyser-mappings.

    python tools/mapgen/fetch_sources.py        # writes tools/mapgen/sources/cache/ (gitignored)
"""

import hashlib
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE / "sources" / "cache"

JAVA_SERVER_URL = "https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar"
JAVA_SERVER_SHA1 = "33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c"  # Minecraft Java 26.3, from Mojang's manifest

GEYSER_COMMIT = "50a464d7db132ca5943b608a2110c158bf09702a"
PALETTE_URL = (f"https://raw.githubusercontent.com/GeyserMC/Geyser/{GEYSER_COMMIT}"
               "/core/src/main/resources/bedrock/block_palette.26_50.nbt")
PALETTE_SHA256 = "54c9bed0bef30aadc316b21c6e82b5ac407c5c6e351b9d9690aa4e9a49fec912"


def download(url, dest, algo, expected):
    if dest.exists() and hashlib.new(algo, dest.read_bytes()).hexdigest() == expected:
        print(f"ok (cached)  {dest.name}")
        return
    print(f"downloading  {url}")
    data = urllib.request.urlopen(url, timeout=120).read()
    digest = hashlib.new(algo, data).hexdigest()
    if digest != expected:
        sys.exit(f"hash mismatch for {url}: {digest} != {expected}")
    dest.write_bytes(data)
    print(f"ok           {dest.name}")


def main():
    CACHE.mkdir(parents=True, exist_ok=True)
    download(PALETTE_URL, CACHE / "block_palette.26_50.nbt", "sha256", PALETTE_SHA256)

    jar = CACHE / "server-26.3.jar"
    download(JAVA_SERVER_URL, jar, "sha1", JAVA_SERVER_SHA1)
    reports = CACHE / "java-26.3-generated"
    if not (reports / "reports" / "blocks.json").exists():
        print("running Mojang's data generator (reports only)...")
        subprocess.run(["java", "-DbundlerMainClass=net.minecraft.data.Main", "-jar", str(jar),
                        "--reports", "--output", str(reports)], cwd=CACHE, check=True,
                       stdout=subprocess.DEVNULL)
    # version.json (id, world_version = DataVersion) sits at the root of the server jar.
    with zipfile.ZipFile(jar) as z:
        (reports / "version.json").write_bytes(z.read("version.json"))
    print(f"ok           {reports / 'reports' / 'blocks.json'}")


if __name__ == "__main__":
    main()
