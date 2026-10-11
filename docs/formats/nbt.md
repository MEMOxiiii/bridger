# NBT (Named Binary Tag)

Editions covered: Java (big-endian) and Bedrock (little-endian) file NBT. Bedrock's network NBT (varint lengths)
and Java's nameless network root are **not** implemented. No file format Bridger supports uses them yet.

Implementation: `src/io/include/bridger/io/nbt/` (`nbt_view.hpp` zero-copy reader, `nbt_value.hpp` owning tree,
`nbt_writer.hpp`, `mutf8.hpp`).

## Container

A document is one **named root tag**: type id (1 byte), name length (unsigned 16-bit), name bytes, payload [1].
In practice the root is a compound. Bridger accepts any non-End root type. Bytes after the root are allowed and
reported (`NbtDocument::consumed_bytes()`), because some containers prefix or suffix NBT.

Byte order: Java uses big-endian for every number, including the name length [1]. Bedrock uses little-endian for
"all numbers ... This includes the size prefix before tag names" [1].

## Tag types [1]

| Id | Tag | Payload |
|---|---|---|
| 0 | End | none; terminates a compound; element type of empty lists |
| 1 | Byte | 1 byte, signed |
| 2 | Short | 2 bytes, signed |
| 3 | Int | 4 bytes, signed |
| 4 | Long | 8 bytes, signed |
| 5 | Float | IEEE 754 binary32 |
| 6 | Double | IEEE 754 binary64 |
| 7 | Byte_Array | Int length (signed), then that many bytes |
| 8 | String | unsigned Short byte length, then modified UTF-8 bytes (no terminator) |
| 9 | List | element type id (1 byte), Int length, then that many payloads without ids or names |
| 10 | Compound | named tags, then a TAG_End |
| 11 | Int_Array | Int length, then that many Int payloads |
| 12 | Long_Array | Int length, then that many Long payloads |

## Strings: modified UTF-8

Java writes NBT strings with `DataOutput.writeUTF` ("modified UTF-8") [2]. It differs from UTF-8 in two ways:
U+0000 is encoded as `C0 80`, and characters above U+FFFF are encoded as a UTF-16 surrogate pair, each half as a
3-byte sequence [2]. Bridger keeps strings in their **raw on-disk form**, which makes round-trips byte-exact, and
converts them only when needed (`mutf8_to_utf8`, `utf8_to_mutf8`). Decoding is as lenient as
`DataInputStream.readUTF` [2]: a raw NUL byte and overlong forms are accepted. Unpaired surrogates (legal in Java
strings, not in UTF-8) become U+FFFD and are flagged as lossy.

ASCII names and ids are identical in both encodings, so lookups like `find("Palette")` compare raw bytes.

## Lists

- An empty list may declare element type End or any other type ("empty lists may be represented as a list of Byte
  tags rather than a list of the correct type, or as a list of End tags in newer versions") [1]. Bridger keeps the
  declared type for byte-exact round-trips.
- A **non-empty** list with element type End is rejected (`Malformed`): its elements would have no payload, and
  the game itself treats it as invalid.
- Negative lengths (lists and arrays) are rejected (`Malformed`).

## Limits we enforce on input

| Limit | Value | Error |
|---|---|---|
| Nesting depth (compounds + lists) | 512, a hard cap (`kMaxNbtDepth`): "Compound and List tags may not be nested beyond a depth of 512" [1] | `LimitExceeded` |
| Total tag count | 2^28 by default (`NbtLimits::max_tags`) | `LimitExceeded` |
| Every length | must fit in the remaining bytes (checked with 64-bit arithmetic, before any loop) | `UnexpectedEof` |

Before walking a list, its claimed count × the smallest possible element size is checked against the remaining
bytes. A 13-byte file claiming 2^31 compounds fails immediately.

**No recursion on untrusted depth.** Validation, skipping, materializing, writing, comparing and destroying trees
all use explicit stacks: a heap vector, or a fixed array of `kMaxNbtDepth` entries. A legal 512-deep document
therefore cannot overflow the C++ call stack in any build. This was found by the ASan build and fixed in Phase 1.
Copying an `NbtValue` is still recursive; it is only used on trusted, shallow data.

## Round-trip guarantee

Parsing a valid document and writing it back with the same byte order yields **identical bytes**: entry order,
duplicate keys, empty-list element types, raw strings and float bit patterns (including NaN payloads) are all
preserved. Verified by:
- `tests/unit/io/nbt_test.cpp` on reference files written by an independent implementation (nbtlib [3]) in both
  byte orders;
- the `fuzz_nbt` target, which aborts if any accepted input does not round-trip byte-exactly.

## Sources

1. minecraft.wiki, "NBT format": https://minecraft.wiki/w/NBT_format (checked 2026-10-11)
2. Oracle, Java SE API, `java.io.DataInput` ("Modified UTF-8") and `DataInputStream.readUTF`:
   https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/io/DataInput.html
3. nbtlib 2.0.4 (MIT), used only to generate test data: https://github.com/vberlier/nbtlib
