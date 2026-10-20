# Data-safety guarantees

> **Status (Phase 6):** a guarantee marked **implemented** below has code and tests behind it. Anything else is
> a design commitment, not a fact about the shipped binary.

| Guarantee | Status |
|---|---|
| Inputs are never modified | **implemented**: inputs are memory-mapped read-only; a test checks the input bytes are unchanged after conversion. `--into-world` reads the existing world and writes a new one; a test checks the existing world's bytes are unchanged |
| Same-directory temp file → flush → validate → atomic move | **implemented**: `platform::AtomicFileWriter` (`src/platform/atomic_replace.cpp`) |
| Recovery procedure on replace failure | **implemented and failure-tested**: `tests/unit/platform/file_test.cpp` injects a failure at every step (create, write, flush, close, rename, each `ReplaceFileW` error code, failed restore, destination failing validation) |
| No silent data loss (conversion report) | **implemented** for every conversion path: unmapped blocks (with fallback), lossy data, unsupported block entities, entities (not written yet), biomes, clipped blocks, block entities/entities removed by placement; reader warnings (skipped palette variants, structure void, unreadable or partially generated chunks, dimensions not converted) are shown and written to `--report` too |
| Output validated before it replaces anything | **implemented**: new worlds are unzipped, their LevelDB opened, every subchunk decoded and the counts compared with what was written; `.mcstructure` files are re-read; placed worlds are compared key by key with the source (below); converted Java worlds are validated as a directory before zipping and the zip listing is checked before the move |
| Untouched world data copied byte-for-byte | **implemented**: `validate_placed_world` checks every LevelDB key the placement did not touch is byte-identical to the source world and `level.dat` is unchanged; a test tampers with an untouched entry and expects the validation to fail |
| Existing output is replaced only on request | **implemented**: `--overwrite` is required; without it the conversion stops before writing. A Java world **folder** is never replaced, even with `--overwrite` |
| Java world folders written atomically | **implemented**: built in a temp folder next to the destination, validated (every chunk re-read), then renamed into place with `MoveFileExW` (no replace); on failure the temp folder is removed and the destination was never touched |
| Untrusted-input limits | **implemented** for decompression, NBT, zip and every reader (`.schem`, `.nbt`, `.litematic`, `.schematic`, `.mcstructure`, Anvil chunks and region files; see `docs/formats/`); fuzzed |

## 1. Inputs are never modified

Bridger opens every input read-only (Bedrock worlds read for conversion to Java are unzipped into a scratch
directory and only the copy is opened). That includes worlds used as a placement target: `--into-world` reads the
existing world and writes a **new** output world. The original is never edited in place: it is unzipped into a
scratch directory under `%TEMP%`, the copy is changed there, and the result is zipped into the output. Java world
folders are only read. Before anything is read, a conversion is refused, even with `--overwrite`, when its output is the input,
lies inside an input world folder, or is a folder that contains the input (`check_output_is_not_input` in
`convert/pipeline.cpp`; possible now that a world converts to its own edition). Tests:
`tests/unit/translate/targets_test.cpp` ("A conversion never writes over or into its input").

## 2. How output is written

1. The output is written to a temporary file **in the same directory as the destination**, so the final rename
   never crosses volumes. The name is unique and ends in `.bridger-tmp`.
2. The temp file is flushed to disk (`FlushFileBuffers`).
3. The temp file is **re-read and validated** by Bridger's own reader. This catches crashes and structural
   errors. It is **not** proof of correctness, because writer and reader can share a mistake (see
   `docs/testing.md`).
4. It is moved into place:
   - **Destination doesn't exist** (normal case): `MoveFileExW(temp, dest, MOVEFILE_WRITE_THROUGH)`, without
     `MOVEFILE_REPLACE_EXISTING`. If the move fails, nothing existed before, so nothing is lost. The temp file is
     kept and its path is reported.
   - **Destination exists** (the user chose to overwrite an earlier output): `ReplaceFileW(dest, temp, backup)`,
     **always with a backup name**. Each documented failure code is handled explicitly. The descriptions below
     paraphrase Microsoft's `ReplaceFileW` reference for the case where a backup name is given. Phase 1
     re-checks them against the docs and failure-injection tests.
     - `ERROR_UNABLE_TO_REMOVE_REPLACED` (1175): both files keep their original names. The destination is
       unchanged. The temp file is kept and reported.
     - `ERROR_UNABLE_TO_MOVE_REPLACEMENT` (1176): the replacement could not be renamed, and both files keep their
       original names. The destination is unchanged. The temp file is kept and reported.
     - `ERROR_UNABLE_TO_MOVE_REPLACEMENT_2` (1177): the replacement could not be moved. The **original is now
       under the backup name** and the destination name is free. Bridger tries to move the backup back to the
       destination name.
     - Any other error: Bridger checks which of temp, backup and destination exist and reports it.
   - The temp file and the backup are **never deleted until the destination is confirmed present and valid**.
5. The backup is removed only after the final destination passes validation.
6. If automatic recovery fails, Bridger stops and prints exactly which file is where: temp, backup, destination.

**Linux and macOS** (`src/platform/posix/file_ops_posix.cpp`) follow the same steps with the POSIX calls:

- Step 2 is `fsync` on the temp file. A rename is durable only once its directory is synced, so every rename below
  is followed by `fsync` on the destination's folder.
- New destination: `renameat2(..., RENAME_NOREPLACE)` (Linux 3.15+) or `renamex_np(..., RENAME_EXCL)` (macOS 10.12+),
  which fail without touching anything if the name exists. On a file system without them (`EINVAL`, `ENOTSUP`):
  `link()` + `unlink()` for a file (also refuses an existing name), and for a Java world folder a check that the name
  is free right before `rename()` (a folder created in that instant would be replaced only if it is empty).
- Existing destination: the old file gets the backup name as a hard link first (`link()`), then the temp file takes
  the destination's name with one `rename()`, which POSIX defines as atomic. If the link fails, nothing changed
  (`RemoveReplacedFailed`); if the rename fails, the destination is unchanged (`MoveReplacementFailed`).

**Cancelling** (the desktop app's Cancel, `ConvertOptions::cancel`): the request is checked between steps and
chunk batches, and **one last time after the output was validated, immediately before step 4**: a separate
gate (`platform::MoveGate`) that the atomic writer calls exactly once, before the move, and never again after it
(so a late cancel cannot undo a finished replacement); for Java worlds, just before the folder rename. A cancel up to that moment discards the temp
file or temp folder and leaves the destination untouched; the conversion ends with `ErrorCode::Cancelled`. The
move in step 4 is the point of no return: a cancel that arrives during or after it has no effect, and the
conversion reports success with the finished file. Tests: `tests/unit/convert/cancel_test.cpp` (every output
kind, cancelled during the last step, also when replacing an existing file with `--overwrite`) and
`tests/unit/platform/file_test.cpp` (the gate runs once; a cancel after the replacement does not undo it).

**The report file** (`--report`) is written the same way, and Bridger refuses, before converting, a report path
that is the input, the output, a path inside an output world folder, or the world given to `--into-world`. An
existing report file is replaced only with `--overwrite`.

## 3. Limits of these guarantees

What the OS does and does not promise:

- `MoveFileExW` / `ReplaceFileW` on the same NTFS volume are the best Windows offers. Microsoft does not formally
  document them as atomic under every failure (power loss, network shares, FAT32/exFAT, antivirus interference).
  Bridger does not claim more.
- On network drives (SMB) and non-NTFS file systems, atomicity and write-through behave differently or are
  absent.
- `FlushFileBuffers` relies on the drive honouring flush requests. Drives with volatile write caches that ignore
  flushes can still lose data on power loss.
- On Linux and macOS, `fsync` has the same dependency on the drive. On macOS it does not force the drive's own
  cache (`F_FULLFSYNC` would); Bridger uses plain `fsync`, like most programs. Hard links (the backup when
  overwriting) need a file system that has them; on FAT32/exFAT overwriting fails before anything changes.
- A `.mcworld` is a zip. The guarantees cover the zip file as a whole, not its internal entries.
- Java world output is a folder of many files. The folder appears at its destination in one rename, but the files
  inside are written with ordinary writes before that rename; Bridger does not flush each one individually.
- Converted Java worlds are zipped by minizip-ng straight into the temp file (no full copy in memory). The temp
  file is then flushed by path, validated and moved like any other output.

## 4. No silent data loss

Every block, block entity, entity or biome that cannot be translated exactly is counted in the
`ConversionReport`. That covers the source state, the count, and example positions. A configurable fallback is
written in its place (`--fallback air|marker`). The conversion ends with a report, and `--report report.json`
writes it to a file. Data that is skipped as a whole (other dimensions, entities, partially generated chunks)
is listed as warnings; the report's `"exact": true` means **no report entry and no warning**, and
`"blocks_exact"` tells whether the blocks alone were exact. The desktop app follows the same rule. Placement removals (overwritten block entities, `--clear-entities`, `--clip`) are reported
the same way.

## 5. Placement policy (writing a structure into a world)

| Situation | Default | Options |
|---|---|---|
| Air in the structure | **keep** existing world blocks | `--air replace` |
| Existing block entities/entities in the area | **kept** unless a structure block overwrites their position; removals reported | `--clear-entities` (reported) |
| Structure exceeds build height | **fail** with an error showing the overflow | `--clip` (dropped blocks reported), `--shift-to-fit` |
| Chunks/keys the structure does not touch | **copied byte-for-byte** | — |
| Placement position | new world: centred on x=0, z=0, on the flat surface; existing world: **no default**, `--place-at` is required | `--place-at x,y,z` |

Changing any default here is a data-safety change and needs maintainer approval.

## 6. Network access

The command-line tool never uses the network. The desktop app uses it for two things only, both described here.

**Update check** (the maintainer's decision, 2026-10-17; details in `docs/app/updates.md`): at most once a day at
start, and only while Settings → Updates → "Check for a new version once a day" is on (the default; off stops every
automatic request), the app asks `https://api.github.com/repos/MEMOxiiii/bridger/releases/latest`. An update is
downloaded only when the user presses **Download and install**, only from this repository's GitHub release downloads,
and installed only after its SHA-256 matches the digest GitHub publishes for it (or the release's `.sha256` file);
without a checksum it is refused. The new files are swapped in with rollback; user data outside the program folder is
never touched.

**Textures**: when the textures for the 3D preview are missing, the user can press **Download from Mojang**. Then
(`src/render/asset_download.cpp`, `src/platform/win32/net_win32.cpp`, `src/platform/posix/net_posix.cpp`):

- Only `https://` URLs on Mojang's download hosts are contacted (`piston-meta.mojang.com`,
  `piston-data.mojang.com`, `launchermeta.mojang.com`, `launcher.mojang.com`); every URL, including those read
  from Mojang's own files, is checked against that list, and redirects to plain http are refused.
- The version file is checked against the SHA-1 in Mojang's manifest, and the client jar against the SHA-1 and size
  in the version file. Sizes are capped (manifest 8 MB, version file 4 MB, jar 256 MB).
- The jar is saved with the atomic writer to `<application data>/Bridger/assets/<version>/client.jar`
  (`%APPDATA%` on Windows, `~/.config` on Linux, `~/Library/Application Support` on macOS) after it opened as a
  client jar with block models. `.minecraft` is never written. The jar is read as a zip for textures and models
  and is **never executed**.
- Cancelling stops the download; nothing is saved.

Windows' own components do the work (WinHTTP for HTTPS, BCrypt for SHA-1 and SHA-256); no third-party networking
code is shipped. On Linux and macOS the system's libcurl does HTTPS (loaded at run time from `libcurl.so.4` or
`/usr/lib/libcurl.4.dylib`, with certificate checks left at libcurl's defaults; only `https` is allowed, for the first
request and every redirect), and Bridger's own SHA-1/SHA-256 code (`src/platform/hash.cpp`, tested against the NIST
examples) does the hashing. Without libcurl the two downloads report that and nothing else changes. The desktop app on
Linux and macOS checks for updates the same way but only offers the release page; it never replaces its own files
(`docs/adr/0010`).

## 7. Untrusted input

Every read is bounds-checked. There are explicit limits on NBT nesting depth, total decompressed size (zip and
deflate bombs), array lengths, palette sizes and structure dimensions. Exceeding one yields
`ErrorCode::LimitExceeded`, never a crash. Fuzz targets cover each reader (`fuzz/`).

## Sources

1. Microsoft Learn, `ReplaceFileW` function (winbase.h): https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-replacefilew
2. Microsoft Learn, `MoveFileExW` function (winbase.h): https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-movefileexw
3. Microsoft Learn, `FlushFileBuffers` function (fileapi.h): https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers
