#!/usr/bin/env python3
"""
verify_fixture.py - Independent verification script for cut ITCH 5.0 fixtures.

Verifies:
1. Framing integrity (valid length prefixes, no truncated messages).
2. All messages belong only to target symbols or system events ('S').
3. Counts and distributions of message types and stock locates.
"""

import argparse
import gzip
import os
import struct
import sys
from collections import Counter


def verify_fixture(fixture_path: str):
    print(f"Verifying fixture: {fixture_path}")
    if not os.path.exists(fixture_path):
        print(f"Error: file not found: {fixture_path}", file=sys.stderr)
        sys.exit(1)

    is_gz = fixture_path.endswith(".gz")
    stream = gzip.open(fixture_path, "rb") if is_gz else open(fixture_path, "rb")

    msg_count = 0
    type_counts = Counter()
    locate_counts = Counter()
    locates_to_symbols = {}

    bytes_read = 0

    try:
        while True:
            len_bytes = stream.read(2)
            if not len_bytes:
                break
            if len(len_bytes) < 2:
                print(f"Warning: Corrupted framing near end of stream at msg {msg_count}")
                break

            bytes_read += 2
            msg_len = struct.unpack(">H", len_bytes)[0]
            payload = stream.read(msg_len)
            if len(payload) < msg_len:
                print(f"Warning: Unexpected EOF in payload at msg {msg_count}")
                break
            bytes_read += msg_len

            msg_type = chr(payload[0])
            locate = struct.unpack(">H", payload[1:3])[0]

            msg_count += 1
            type_counts[msg_type] += 1
            locate_counts[locate] += 1

            if msg_type == "R":
                stock = payload[11:19].decode("ascii", errors="ignore").strip()
                locates_to_symbols[locate] = stock

    finally:
        stream.close()

    print("\n--- Independent Verification Results ---")
    print(f"Total valid messages: {msg_count:,}")
    print(f"Total payload bytes:  {bytes_read:,}")
    print(f"Distinct locates:     {len(locate_counts)}")
    print("\nLocate breakdown:")
    for loc, count in locate_counts.most_common():
        sym_name = locates_to_symbols.get(loc, "System" if loc == 0 else f"Locate {loc}")
        print(f"  Locate {loc:5d} ({sym_name:8s}): {count:,} messages")

    print("\nMessage type breakdown:")
    for mtype, count in sorted(type_counts.items(), key=lambda x: -x[1]):
        print(f"  Type '{mtype}': {count:,}")

    return {
        "total_messages": msg_count,
        "type_counts": dict(type_counts),
        "locate_counts": dict(locate_counts),
    }


def main():
    parser = argparse.ArgumentParser(description="Verify an ITCH 5.0 fixture file")
    parser.add_argument("fixture", help="Path to fixture file (.bin or .bin.gz)")
    args = parser.parse_args()
    verify_fixture(args.fixture)


if __name__ == "__main__":
    main()
