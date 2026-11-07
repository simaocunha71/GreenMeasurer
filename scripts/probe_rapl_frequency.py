#!/usr/bin/env python3
"""Measure observed changes in the Intel package RAPL counter."""

import argparse
import math
import os
import statistics
import struct
import sys
import time

from analyze_overtime import percentile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=5.0, help="Duration in seconds (default: 5)")
    parser.add_argument("--core", type=int, default=0, help="CPU core whose MSR device to read (default: 0)")
    args = parser.parse_args()
    if not math.isfinite(args.duration) or args.duration <= 0 or args.core < 0:
        parser.error("duration must be finite and positive and core must be non-negative")

    try:
        fd = os.open(f"/dev/cpu/{args.core}/msr", os.O_RDONLY)
        try:
            def read_counter():
                raw = os.pread(fd, 8, 0x611)
                if len(raw) != 8:
                    raise OSError("short MSR read")
                return struct.unpack("<Q", raw)[0] & 0xFFFFFFFF

            previous = read_counter()
            started = last = time.monotonic_ns()
            deadline = started + int(args.duration * 1_000_000_000)
            intervals = []
            while time.monotonic_ns() < deadline:
                current = read_counter()
                now = time.monotonic_ns()
                if current != previous:
                    intervals.append((now - last) / 1_000_000)
                    previous, last = current, now
            elapsed = (time.monotonic_ns() - started) / 1_000_000_000
        finally:
            os.close(fd)
    except OSError as error:
        print(f"Error reading RAPL MSR: {error}", file=sys.stderr)
        return 1
    if not intervals:
        print("Error: no counter changes observed", file=sys.stderr)
        return 1
    print(f"Observed changes: {len(intervals)} in {elapsed:.3f} s")
    print(f"Median: {statistics.median(intervals):.6f} ms; p95: {percentile(intervals, 0.95):.6f} ms")
    print(f"Minimum: {min(intervals):.6f} ms; maximum: {max(intervals):.6f} ms")
    print("Polling consumes CPU; observed changes do not guarantee that all hardware updates were seen.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
