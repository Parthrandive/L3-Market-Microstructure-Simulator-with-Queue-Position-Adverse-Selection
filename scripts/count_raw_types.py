#!/usr/bin/env python3
"""
count_raw_types.py - Minimal, completely independent raw ITCH 5.0 type counter.

Written with zero shared code/helpers to independently verify message counts,
per-symbol distributions, and message types directly from raw ITCH 5.0 byte streams.
"""

import gzip
import sys
from collections import Counter, defaultdict

BUFFER_CHUNK = 8 * 1024 * 1024  # 8 MB buffer


def count_raw_stream(filepath, max_messages=0):
    is_gz = filepath.endswith(".gz")
    f = gzip.open(filepath, "rb") if is_gz else open(filepath, "rb")

    type_counts = Counter()
    locate_counts = Counter()
    locate_type_counts = defaultdict(Counter)
    locate_to_symbol = {}
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
                locate_type_counts[locate][msg_type] += 1

                if msg_type == "R" and msg_len >= 19:
                    stock = buf[pos + 13 : pos + 21].decode("ascii", errors="ignore").strip()
                    locate_to_symbol[locate] = stock

                pos = rec_end
                if max_messages and total >= max_messages:
                    break

            del buf[:pos]
            if max_messages and total >= max_messages:
                break
    finally:
        f.close()

    print(f"Total messages read: {total:,}")
    print("\nAggregate counts by message type:")
    for t, c in sorted(type_counts.items(), key=lambda x: -x[1]):
        print(f"  '{t}': {c:,}")

    print("\nPer-Symbol Message Breakdown (Independent Verification):")
    header = f"{'Locate':<8} {'Symbol':<8} {'Total':<10} {'Adds(A+F)':<11} {'Replaces(U)':<12} {'Deletes(D)':<11} {'Execs(E+C)':<11} {'Cancels(X)':<11}"
    print(header)
    print("-" * len(header))

    for loc, count in locate_counts.most_common():
        sym = locate_to_symbol.get(loc, "System" if loc == 0 else f"Loc_{loc}")
        tc = locate_type_counts[loc]
        adds = tc["A"] + tc["F"]
        replaces = tc["U"]
        deletes = tc["D"]
        execs = tc["E"] + tc["C"]
        cancels = tc["X"]
        print(f"{loc:<8} {sym:<8} {count:<10,} {adds:<11,} {replaces:<12,} {deletes:<11,} {execs:<11,} {cancels:<11,}")

    return total, type_counts, locate_counts, locate_type_counts


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "data/fixture_3symbols.bin.gz"
    count_raw_stream(path)
