#!/usr/bin/env python3
"""Rewrite an MP4's 32-bit chunk offset tables as 64-bit ones.

Files over 4 GB use `co64` instead of `stco`, and the parser has a separate branch for it that no
test could otherwise reach - generating a 4 GB fixture to exercise ten lines of code is not a
trade anyone should make. This widens the existing table instead: same offsets, same samples, a
file that ffprobe still reads identically.

It relies on `moov` sitting after `mdat`, which is where ffmpeg puts it without `+faststart`.
Growing `moov` then cannot move the sample data, so every chunk offset stays correct. The script
checks that and refuses rather than writing a subtly broken file.
"""
import struct
import sys

CONTAINERS = {"moov", "trak", "mdia", "minf", "stbl", "udta", "meta", "edts"}


def walk(data, offset, end, path=()):
    """Every box in [offset, end), each with the chain of containers above it."""
    found = []
    while offset + 8 <= end:
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8].decode("latin1")
        if size < 8 or offset + size > end:
            break
        found.append((kind, offset, size, path))
        if kind in CONTAINERS:
            # `meta` is a FullBox: four bytes of version and flags before its children.
            child = offset + 8 + (4 if kind == "meta" else 0)
            found += walk(data, child, offset + size, path + ((kind, offset),))
        offset += size
    return found


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <in.mp4> <out.mp4>", file=sys.stderr)
        return 2

    data = bytearray(open(sys.argv[1], "rb").read())

    top = [box for box in walk(data, 0, len(data)) if not box[3]]
    order = [kind for kind, _, _, _ in top]
    if "moov" not in order or "mdat" not in order:
        print("not an MP4 with both moov and mdat", file=sys.stderr)
        return 1
    if order.index("moov") < order.index("mdat"):
        print("moov precedes mdat; widening it would move every sample", file=sys.stderr)
        return 1

    # Back to front, so rewriting one table does not move the ones not yet done.
    tables = sorted(
        (box for box in walk(data, 0, len(data)) if box[0] == "stco"),
        key=lambda box: -box[1],
    )
    if not tables:
        print("no stco to widen", file=sys.stderr)
        return 1

    for _, offset, size, path in tables:
        count = struct.unpack_from(">I", data, offset + 12)[0]
        offsets = [
            struct.unpack_from(">I", data, offset + 16 + index * 4)[0] for index in range(count)
        ]

        widened = bytearray()
        widened += struct.pack(">I", 16 + count * 8)
        widened += b"co64"
        widened += data[offset + 8 : offset + 16]  # version, flags, entry_count
        for value in offsets:
            widened += struct.pack(">Q", value)

        delta = len(widened) - size
        data[offset : offset + size] = widened
        for _, parent in path:
            struct.pack_into(">I", data, parent, struct.unpack_from(">I", data, parent)[0] + delta)

        print(f"stco at {offset}: {count} entries, {size} -> {len(widened)} bytes")

    open(sys.argv[2], "wb").write(bytes(data))
    print(f"wrote {sys.argv[2]} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
