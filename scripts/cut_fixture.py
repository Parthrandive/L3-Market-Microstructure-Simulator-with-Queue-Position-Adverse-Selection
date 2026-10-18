#!/usr/bin/env python3
"""
cut_fixture.py - High-speed extraction of 3-symbol ITCH 5.0 fixture (AAPL, SPY, NVDA).

Streams compressed ITCH 5.0 data (from a local file or HTTP URL) without unpacking
the multi-gigabyte file onto disk. Uses a large block-buffered framing reader
to achieve millions of messages per second.
"""

import argparse
import gzip
import os
import struct
import sys
import time
import urllib.request
from collections import Counter
from typing import Dict, Set

BUFFER_CHUNK = 8 * 1024 * 1024  # 8 MB read buffer


def open_input_stream(source: str):
    """Opens a local file or HTTP URL as a gzipped binary stream."""
    if source.startswith("http://") or source.startswith("https://"):
        req = urllib.request.Request(source, headers={"User-Agent": "Mozilla/5.0"})
        resp = urllib.request.urlopen(req)
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
    progress_interval: int = 25_000_000,
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
    buf = bytearray()

    try:
        while True:
            chunk = stream.read(BUFFER_CHUNK)
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

                total_read += 1
                payload_start = pos + 2
                msg_type_byte = buf[payload_start]
                msg_type = chr(msg_type_byte)
                locate = (buf[payload_start + 1] << 8) | buf[payload_start + 2]

                # Handle Stock Directory ('R') to map locate code -> symbol
                if msg_type == "R":
                    stock_bytes = buf[payload_start + 11 : payload_start + 19]
                    stock = stock_bytes.decode("ascii", errors="ignore").strip()
                    locate_to_symbol[locate] = stock
                    if stock in target_symbols:
                        symbol_to_locate[stock] = locate
                        target_locates.add(locate)
                        print(f"  [Directory] Found target {stock:8s} -> locate {locate}")

                keep = False
                if msg_type == "S":
                    keep = True
                    counts_by_symbol["SYSTEM"] += 1
                elif msg_type == "R":
                    if keep_all_r or locate in target_locates:
                        keep = True
                        stock = locate_to_symbol.get(locate, "UNKNOWN")
                        counts_by_symbol[stock] += 1
                else:
                    if locate in target_locates:
                        keep = True
                        stock = locate_to_symbol.get(locate, f"LOC_{locate}")
                        counts_by_symbol[stock] += 1

                if keep:
                    out_file.write(buf[pos:rec_end])
                    total_written += 1
                    counts_by_type[msg_type] += 1

                pos = rec_end

                if total_read % progress_interval == 0:
                    elapsed = time.time() - start_time
                    mps = total_read / elapsed if elapsed > 0 else 0
                    print(
                        f"Processed {total_read:,} messages ({mps:,.0f} msg/s) | "
                        f"Kept {total_written:,} | Target locates found: {len(target_locates)}/{len(target_symbols)}",
                        flush=True,
                    )

                if max_messages > 0 and total_read >= max_messages:
                    break

            del buf[:pos]
            if max_messages > 0 and total_read >= max_messages:
                break

    finally:
        out_file.close()
        raw_handle.close()

    elapsed = time.time() - start_time
    print("\n--- Cutting Summary ---")
    print(f"Time taken: {elapsed:.2f} s ({total_read / elapsed:,.0f} msg/s)")
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
        default="data/raw/12302019.NASDAQ_ITCH50.gz",
        help="Input file (.gz or .bin) or HTTP URL",
    )
    parser.add_argument(
        "--output",
        default="data/fixture_3symbols_full.bin.gz",
        help="Output fixture path (e.g. data/fixture_3symbols_full.bin.gz)",
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
