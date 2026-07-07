#!/usr/bin/env python3
"""hyphc — compiles TeX/hyph-utf8 hyphenation pattern text into FreeInkBook's
FIBH binary format.

Input: one pattern per line (e.g. ".ach4", "ab1o"); '%' starts a comment.
Output layout (little-endian):
  u32 magic 'FIBH' | u16 version=1 | u8 maxPatLen | u8 reserved
  u32 patternCount
  u32 offsets[patternCount]            offsets into the record blob
  records: per pattern — u8 keyLen, key bytes (no digits, lowercase),
           u8 values[keyLen + 1] (Liang inter-character values)
Patterns are sorted bytewise by key so the matcher can binary-search
incrementally per added character.

Usage: hyphc.py <patterns.txt> <out.fibh>
"""
import struct
import sys


def compile_patterns(lines):
    table = {}
    max_len = 0
    for raw in lines:
        line = raw.split("%", 1)[0].strip()
        if not line:
            continue
        key = []
        values = [0]
        for ch in line:
            if ch.isdigit():
                values[-1] = int(ch)
            else:
                key.append(ch.lower())
                values.append(0)
        key_bytes = "".join(key).encode("utf-8")
        if not key_bytes or len(key_bytes) > 255:
            continue
        # values has len(key)+1 entries; keep the max on duplicate keys.
        old = table.get(key_bytes)
        if old is None:
            table[key_bytes] = values
        else:
            table[key_bytes] = [max(a, b) for a, b in zip(old, values)]
        max_len = max(max_len, len(key_bytes))
    return sorted(table.items()), max_len


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        patterns, max_len = compile_patterns(f)

    blob = bytearray()
    offsets = []
    for key, values in patterns:
        offsets.append(len(blob))
        blob.append(len(key))
        blob.extend(key)
        blob.extend(values)

    with open(sys.argv[2], "wb") as out:
        out.write(b"FIBH")
        out.write(struct.pack("<HBB", 1, max_len, 0))
        out.write(struct.pack("<I", len(patterns)))
        out.write(struct.pack(f"<{len(offsets)}I", *offsets))
        out.write(blob)

    size = 12 + 4 * len(offsets) + len(blob)
    print(f"hyphc: {len(patterns)} patterns, max key {max_len}, {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
