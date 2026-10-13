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
folders are only read.

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

**Cancelling** (the desktop app's Cancel, `ConvertOptions::cancel`): the request is checked between steps and
chunk batches, and **one last time after the output was validated, immediately before step 4** (inside the
validator for files; just before the folder rename for Java worlds). A cancel up to that moment discards the temp
file or temp folder and leaves the destination untouched; the conversion ends with `ErrorCode::Cancelled`. The
move in step 4 is the point of no return: a cancel that arrives during or after it has no effect, and the
conversion reports success with the finished file. Tests: `tests/unit/convert/cancel_test.cpp` (every output
kind, cancelled during the last step).

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

## 6. Untrusted input

Every read is bounds-checked. There are explicit limits on NBT nesting depth, total decompressed size (zip and
deflate bombs), array lengths, palette sizes and structure dimensions. Exceeding one yields
`ErrorCode::LimitExceeded`, never a crash. Fuzz targets cover each reader (`fuzz/`).

## Sources

1. Microsoft Learn, `ReplaceFileW` function (winbase.h): https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-replacefilew
2. Microsoft Learn, `MoveFileExW` function (winbase.h): https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-movefileexw
3. Microsoft Learn, `FlushFileBuffers` function (fileapi.h): https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers
