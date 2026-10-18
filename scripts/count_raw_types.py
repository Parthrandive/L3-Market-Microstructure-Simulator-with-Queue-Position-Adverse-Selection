#!/usr/bin/env python3
"""
count_raw_types.py - Minimal, completely independent raw ITCH 5.0 type counter.

Written with zero shared code/helpers to independently verify message counts
and type distributions directly from an ITCH 5.0 byte stream.
"""

import gzip
import struct
import sys
from collections import Counter


def count_raw_stream(filepath, max_messages=0):
    is_gz = filepath.endswith(".gz")
    f = gzip.open(filepath, "rb") if is_gz else open(filepath, "rb")

    type_counts = Counter()
    locate_counts = Counter()
    total = 0

    try:
        while True:
            header = f.read(2)
            if not header or len(header) < 2:
                break
            length = struct.unpack(">H", header)[0]
            payload = f.read(length)
            if len(payload) < length:
                break

            total += 1
            msg_type = chr(payload[0])
            locate = struct.unpack(">H", payload[1:3])[0]

            type_counts[msg_type] += 1
            locate_counts[locate] += 1

            if max_messages and total >= max_messages:
                break
    finally:
        f.close()

    print(f"Total messages read: {total:,}")
    print("\nCounts by message type:")
    for t, c in sorted(type_counts.items(), key=lambda x: -x[1]):
        print(f"  '{t}': {c:,}")

    return total, type_counts, locate_counts


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "data/fixture_3symbols.bin.gz"
    count_raw_stream(path)
