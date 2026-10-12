# Data-safety guarantees

> **Status (Phase 4):** a guarantee marked **implemented** below has code and tests behind it. Anything else is
> a design commitment, not a fact about the shipped binary.

| Guarantee | Status |
|---|---|
| Inputs are never modified | **implemented**: inputs are memory-mapped read-only; a test checks the input bytes are unchanged after conversion. "Into an existing world" (0.2.0) will write a new world, never edit the original |
| Same-directory temp file → flush → validate → atomic move | **implemented**: `platform::AtomicFileWriter` (`src/platform/atomic_replace.cpp`) |
| Recovery procedure on replace failure | **implemented and failure-tested**: `tests/unit/platform/file_test.cpp` injects a failure at every step (create, write, flush, close, rename, each `ReplaceFileW` error code, failed restore, destination failing validation) |
| No silent data loss (conversion report) | **implemented** for `.schem` → `.mcworld`: unmapped blocks (with fallback), lossy data, unsupported block entities, entities (not written yet), biomes, clipped blocks; shown by `bridger convert` and written by `--report` |
| Output validated before it replaces anything | **implemented**: the written `.mcworld` is unzipped, its LevelDB opened, every subchunk decoded and the counts compared with what was written, before the atomic move |
| Existing output is replaced only on request | **implemented**: `--overwrite` is required; without it the conversion stops before writing |
| Untrusted-input limits | **implemented** for decompression, NBT and `.schem` (see `docs/formats/nbt.md`, `sponge-schem.md`); fuzzed |

## 1. Inputs are never modified

Bridger opens every input read-only. That includes worlds used as a placement target: `--into-world` reads the
existing world and writes a **new** output world. The original is never edited in place.

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

## 3. Limits of these guarantees

What the OS does and does not promise:

- `MoveFileExW` / `ReplaceFileW` on the same NTFS volume are the best Windows offers. Microsoft does not formally
  document them as atomic under every failure (power loss, network shares, FAT32/exFAT, antivirus interference).
  Bridger does not claim more.
- On network drives (SMB) and non-NTFS file systems, atomicity and write-through behave differently or are
  absent.
- `FlushFileBuffers` relies on the drive honouring flush requests. Drives with volatile write caches that ignore
  flushes can still lose data on power loss.
- A `.mcworld` is a zip. The guarantees cover the zip file as a whole, not its internal entries.

## 4. No silent data loss

Every block, block entity, entity or biome that cannot be translated exactly is counted in the
`ConversionReport`. That covers the source state, the count, and example positions. A configurable fallback is
written in its place (`--fallback air|marker`). The conversion ends with a report, and `--report report.json`
writes it to a file. Placement removals (overwritten block entities, `--clear-entities`, `--clip`) are reported
the same way.

## 5. Placement policy (writing a structure into a world)

| Situation | Default | Options |
|---|---|---|
| Air in the structure | **keep** existing world blocks | `--air replace` |
| Existing block entities/entities in the area | **kept** unless a structure block overwrites their position; removals reported | `--clear-entities` (reported) |
| Structure exceeds build height | **fail** with an error showing the overflow | `--clip` (dropped blocks reported), `--shift-to-fit` |
| Chunks/keys the structure does not touch | **copied byte-for-byte** | — |
| Placement position | new world: centred on x=0, z=0, on the flat surface | `--place-at x,y,z` |

Changing any default here is a data-safety change and needs maintainer approval.

## 6. Untrusted input

Every read is bounds-checked. There are explicit limits on NBT nesting depth, total decompressed size (zip and
deflate bombs), array lengths, palette sizes and structure dimensions. Exceeding one yields
`ErrorCode::LimitExceeded`, never a crash. Fuzz targets cover each reader (`fuzz/`).

## Sources

1. Microsoft Learn, `ReplaceFileW` function (winbase.h): https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-replacefilew
2. Microsoft Learn, `MoveFileExW` function (winbase.h): https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-movefileexw
3. Microsoft Learn, `FlushFileBuffers` function (fileapi.h): https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers
