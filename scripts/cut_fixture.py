#!/usr/bin/env python3
"""
cut_fixture.py - Cuts a 3-symbol ITCH 5.0 fixture (AAPL, SPY, NVDA) from a Nasdaq ITCH 5.0 feed.

Streams compressed ITCH 5.0 data (from a local file or HTTP URL) without unpacking
the multi-gigabyte file onto disk.

ITCH 5.0 framing:
  Each record begins with a 2-byte big-endian length prefix N,
  followed by N bytes of payload.
  Payload structure:
    Offset 0:    Message Type (1 byte, char)
    Offset 1-2:  Stock Locate (2 bytes, uint16_t, big-endian)
    Offset 3-4:  Tracking Number (2 bytes, uint16_t, big-endian)
    Offset 5-10: Timestamp (6 bytes, uint48_t, big-endian, nanoseconds since midnight)
    ...
  For 'R' (Stock Directory):
    Offset 11-18: Stock Symbol (8 bytes, ASCII, right space-padded)
"""

import argparse
import gzip
import io
import os
import struct
import sys
import time
import urllib.request
from collections import Counter
from typing import Dict, Set


def open_input_stream(source: str):
    """Opens a local file or HTTP URL as a gzipped binary stream."""
    if source.startswith("http://") or source.startswith("https://"):
        req = urllib.request.Request(source, headers={"User-Agent": "Mozilla/5.0"})
        resp = urllib.request.urlopen(req)
        # Wrap response in GzipFile for streaming decompression
        return gzip.GzipFile(fileobj=resp), resp
    else:
        if source.endswith(".gz"):
            f = gzip.open(source, "rb")
        else:
            f = open(source, "rb")
        return f, f


def cut_itch_fixture(
    source: str,
    output_path: str,
    target_symbols: Set[str],
    max_messages: int = 0,
    keep_all_r: bool = False,
    progress_interval: int = 5_000_000,
):
    print(f"Opening source: {source}")
    stream, raw_handle = open_input_stream(source)
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

    is_gz_out = output_path.endswith(".gz")
    out_file = gzip.open(output_path, "wb") if is_gz_out else open(output_path, "wb")

    locate_to_symbol: Dict[int, str] = {}
    symbol_to_locate: Dict[str, int] = {}
    target_locates: Set[int] = set()

    total_read = 0
    total_written = 0
    counts_by_type = Counter()
    counts_by_symbol = Counter()

    start_time = time.time()
    last_log_time = start_time

    try:
        while True:
            len_bytes = stream.read(2)
            if not len_bytes or len(len_bytes) < 2:
                break

            msg_len = struct.unpack(">H", len_bytes)[0]
            payload = stream.read(msg_len)
            if len(payload) < msg_len:
                break

            total_read += 1
            msg_type = chr(payload[0])

            # Handle Stock Directory ('R') to map locate code -> symbol
            if msg_type == "R":
                locate = struct.unpack(">H", payload[1:3])[0]
                stock = payload[11:19].decode("ascii", errors="ignore").strip()
                locate_to_symbol[locate] = stock
                if stock in target_symbols:
                    symbol_to_locate[stock] = locate
                    target_locates.add(locate)
                    print(f"  [Directory] Found target {stock:8s} -> locate {locate}")

            # Determine whether to retain message
            keep = False
            # 1. Keep system events ('S')
            if msg_type == "S":
                keep = True
                counts_by_symbol["SYSTEM"] += 1
            # 2. Keep 'R' directory messages for targets (or all if specified)
            elif msg_type == "R":
                locate = struct.unpack(">H", payload[1:3])[0]
                if keep_all_r or locate in target_locates:
                    keep = True
                    stock = locate_to_symbol.get(locate, "UNKNOWN")
                    counts_by_symbol[stock] += 1
            # 3. Check stock locate for all order book / trading events
            else:
                locate = struct.unpack(">H", payload[1:3])[0]
                if locate in target_locates:
                    keep = True
                    stock = locate_to_symbol.get(locate, f"LOC_{locate}")
                    counts_by_symbol[stock] += 1

            if keep:
                out_file.write(len_bytes)
                out_file.write(payload)
                total_written += 1
                counts_by_type[msg_type] += 1

            if total_read % progress_interval == 0:
                elapsed = time.time() - start_time
                mps = total_read / elapsed if elapsed > 0 else 0
                print(
                    f"Processed {total_read:,} messages ({mps:,.0f} msg/s) | "
                    f"Kept {total_written:,} | Target locates found: {len(target_locates)}/{len(target_symbols)}"
                )

            if max_messages > 0 and total_read >= max_messages:
                print(f"Reached max message limit: {max_messages:,}")
                break

    finally:
        out_file.close()
        raw_handle.close()

    elapsed = time.time() - start_time
    print("\n--- Cutting Summary ---")
    print(f"Time taken: {elapsed:.2f} s")
    print(f"Total messages scanned: {total_read:,}")
    print(f"Total messages kept:    {total_written:,}")
    print(f"Target locate mappings: {symbol_to_locate}")
    print("\nCounts by symbol:")
    for sym, count in counts_by_symbol.most_common():
        print(f"  {sym:10s}: {count:,}")
    print("\nCounts by message type:")
    for mtype, count in counts_by_type.most_common():
        print(f"  Type '{mtype}': {count:,}")
    print(f"Output saved to: {output_path} ({os.path.getsize(output_path):,} bytes)")

    return {
        "total_read": total_read,
        "total_written": total_written,
        "counts_by_symbol": dict(counts_by_symbol),
        "counts_by_type": dict(counts_by_type),
        "locates": symbol_to_locate,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Extract AAPL, SPY, NVDA and system events from ITCH 5.0"
    )
    parser.add_argument(
        "--source",
        default="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz",
        help="Input file (.gz or .bin) or HTTP URL",
    )
    parser.add_argument(
        "--output",
        default="data/fixture_3symbols.bin.gz",
        help="Output fixture path (e.g. data/fixture_3symbols.bin or .gz)",
    )
    parser.add_argument(
        "--symbols",
        default="AAPL,SPY,NVDA",
        help="Comma-separated target symbols",
    )
    parser.add_argument(
        "--max-messages",
        type=int,
        default=0,
        help="Stop after processing N total messages (0 = full stream)",
    )
    parser.add_argument(
        "--keep-all-r",
        action="store_true",
        help="Keep all 'R' (Stock Directory) messages for reference",
    )

    args = parser.parse_args()
    targets = {s.strip().upper() for s in args.symbols.split(",")}
    cut_itch_fixture(
        args.source,
        args.output,
        targets,
        max_messages=args.max_messages,
        keep_all_r=args.keep_all_r,
    )


if __name__ == "__main__":
    main()
