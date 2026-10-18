#!/usr/bin/env python3
"""
download_parallel.py - Multi-threaded HTTP Range downloader for large ITCH feeds.

Overcomes server-side per-connection bandwidth throttling on emi.nasdaq.com
by fetching distinct byte ranges in parallel worker threads with automatic resume.
"""

import argparse
import concurrent.futures
import json
import os
import sys
import time
import urllib.request

CHUNK_SIZE = 4 * 1024 * 1024  # 4 MB blocks


def get_remote_file_size(url: str) -> int:
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"}, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            cl = resp.headers.get("Content-Length")
            if cl:
                return int(cl)
    except Exception:
        pass
    # Fallback to range request
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Mozilla/5.0", "Range": "bytes=0-0"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        cr = resp.headers.get("Content-Range")
        if cr:
            # format: bytes 0-0/3524013057
            return int(cr.split("/")[-1])
    raise RuntimeError(f"Unable to determine file size for {url}")


def download_chunk(url: str, filepath: str, chunk_idx: int, start: int, end: int, retries: int = 5):
    for attempt in range(retries):
        try:
            req = urllib.request.Request(
                url,
                headers={
                    "User-Agent": "Mozilla/5.0",
                    "Range": f"bytes={start}-{end}",
                },
            )
            with urllib.request.urlopen(req, timeout=20) as resp:
                data = resp.read()
                expected = end - start + 1
                if len(data) != expected:
                    raise IOError(f"Truncated chunk: got {len(data)}, expected {expected}")

                # Thread-safe write at specific offset using file descriptor or separate open
                with open(filepath, "r+b") as f:
                    f.seek(start)
                    f.write(data)
                return chunk_idx
        except Exception as e:
            time.sleep(1 + attempt)
            if attempt == retries - 1:
                raise e


def download_file(url: str, output_path: str, max_workers: int = 32):
    total_size = get_remote_file_size(url)
    print(f"Target URL: {url}")
    print(f"Total size: {total_size:,} bytes ({total_size / (1024**3):.2f} GiB)")
    print(f"Output:     {output_path}")
    print(f"Workers:    {max_workers} threads, Chunk size: {CHUNK_SIZE // (1024*1024)} MB")

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    state_file = output_path + ".state.json"

    completed_chunks = set()
    if os.path.exists(state_file) and os.path.exists(output_path):
        try:
            with open(state_file, "r") as sf:
                state = json.load(sf)
                if state.get("total_size") == total_size:
                    completed_chunks = set(state.get("completed_chunks", []))
                    print(f"Resuming download: {len(completed_chunks)} chunks already finished.")
        except Exception:
            pass

    # Ensure output file is preallocated
    if not os.path.exists(output_path) or os.path.getsize(output_path) != total_size:
        print("Pre-allocating destination file...")
        with open(output_path, "wb") as f:
            f.seek(total_size - 1)
            f.write(b"\0")

    chunks = []
    chunk_idx = 0
    curr = 0
    while curr < total_size:
        end = min(curr + CHUNK_SIZE - 1, total_size - 1)
        chunks.append((chunk_idx, curr, end))
        curr = end + 1
        chunk_idx += 1

    remaining_chunks = [c for c in chunks if c[0] not in completed_chunks]
    total_chunks = len(chunks)
    print(f"Chunks total: {total_chunks:,} | Remaining: {len(remaining_chunks):,}")

    if not remaining_chunks:
        print("All chunks already downloaded!")
        return

    start_time = time.time()
    downloaded_bytes = len(completed_chunks) * CHUNK_SIZE
    completed_in_session = 0

    def save_state():
        with open(state_file, "w") as sf:
            json.dump(
                {
                    "total_size": total_size,
                    "completed_chunks": list(completed_chunks),
                },
                sf,
            )

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_to_chunk = {
            executor.submit(download_chunk, url, output_path, c[0], c[1], c[2]): c
            for c in remaining_chunks
        }

        for future in concurrent.futures.as_completed(future_to_chunk):
            c_idx = future.result()
            completed_chunks.add(c_idx)
            completed_in_session += 1

            if completed_in_session % 10 == 0 or completed_in_session == len(remaining_chunks):
                save_state()
                elapsed = time.time() - start_time
                done_bytes = len(completed_chunks) * CHUNK_SIZE
                pct = min(100.0, (len(completed_chunks) / total_chunks) * 100)
                speed = (completed_in_session * CHUNK_SIZE) / elapsed if elapsed > 0 else 0
                rem_seconds = (total_size - done_bytes) / speed if speed > 0 else 0
                print(
                    f"Progress: {pct:5.1f}% ({len(completed_chunks)}/{total_chunks} chunks) | "
                    f"Speed: {speed / (1024*1024):.2f} MB/s | "
                    f"ETA: {rem_seconds / 60:.1f} min",
                    flush=True,
                )

    save_state()
    if os.path.exists(state_file):
        os.remove(state_file)
    print(f"\nDownload completed in {time.time() - start_time:.1f}s!")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Parallel HTTP Range downloader")
    parser.add_argument(
        "--url",
        default="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz",
        help="Source URL",
    )
    parser.add_argument(
        "--output",
        default="data/raw/12302019.NASDAQ_ITCH50.gz",
        help="Target output file",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=32,
        help="Number of parallel worker connections",
    )
    args = parser.parse_args()
    download_file(args.url, args.output, max_workers=args.workers)
