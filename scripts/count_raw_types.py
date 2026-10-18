#!/usr/bin/env python3
"""
count_raw_types.py - Minimal, completely independent raw ITCH 5.0 type counter.

Written with zero shared code/helpers to independently verify message counts
and type distributions directly from an ITCH 5.0 byte stream.
"""

import gzip
import sys
from collections import Counter

BUFFER_CHUNK = 8 * 1024 * 1024  # 8 MB buffer


def count_raw_stream(filepath, max_messages=0):
    is_gz = filepath.endswith(".gz")
    f = gzip.open(filepath, "rb") if is_gz else open(filepath, "rb")

    type_counts = Counter()
    locate_counts = Counter()
    total = 0
    buf = bytearray()

    try:
        while True:
            chunk = f.read(BUFFER_CHUNK)
            if not chunk:
                break
            buf.extend(chunk)

            pos = 0
            blen = len(buf)
            while pos + 2 <= blen:
                msg_len = (buf[pos] << 8) | buf[pos + 1]
                rec_end = pos + 2 + msg_len
                if rec_end > blen:
                    break

                total += 1
                msg_type = chr(buf[pos + 2])
                locate = (buf[pos + 3] << 8) | buf[pos + 4]

                type_counts[msg_type] += 1
                locate_counts[locate] += 1

                pos = rec_end
                if max_messages and total >= max_messages:
                    break

            del buf[:pos]
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
