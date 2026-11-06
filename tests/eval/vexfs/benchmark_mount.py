#!/usr/bin/env python3
"""Small, bounded benchmark shared by native, VexFS, and competitor mounts."""

import argparse
import json
import os
from pathlib import Path
import statistics
import time


def timed(fn):
    start = time.perf_counter()
    value = fn()
    return time.perf_counter() - start, value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--files", type=int, default=1000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--large-mib", type=int, default=16)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    rounds = []
    block = b"v" * (1024 * 1024)

    for run in range(args.rounds):
        root = args.root / f"run-{run}"
        root.mkdir()

        def create_small():
            for index in range(args.files):
                (root / f"f-{index:05d}.txt").write_bytes(b"x\n")

        create_s, _ = timed(create_small)
        list_s, entries = timed(lambda: list(os.scandir(root)))
        names = [entry for entry in entries if entry.name.startswith("f-")]
        appledouble_count = sum(entry.name.startswith("._") for entry in entries)
        assert len(names) == args.files

        def stat_all():
            return sum(entry.stat().st_size for entry in names)

        stat_s, total_size = timed(stat_all)
        assert total_size == args.files * 2
        large_path = root / "large.bin"

        def write_large():
            with large_path.open("wb", buffering=1024 * 1024) as handle:
                for _ in range(args.large_mib):
                    handle.write(block)
                handle.flush()
                os.fsync(handle.fileno())

        write_s, _ = timed(write_large) if args.large_mib else (0.0, None)

        def read_large():
            total = 0
            with large_path.open("rb", buffering=1024 * 1024) as handle:
                while chunk := handle.read(1024 * 1024):
                    total += len(chunk)
            return total

        read_s, read_bytes = timed(read_large) if args.large_mib else (0.0, 0)
        assert read_bytes == args.large_mib * 1024 * 1024
        rounds.append({
            "create_seconds": create_s,
            "create_files_per_second": args.files / create_s,
            "list_seconds": list_s,
            "listed_entries": len(entries),
            "appledouble_entries": appledouble_count,
            "stat_seconds": stat_s,
            "write_seconds": write_s,
            "write_mib_per_second": args.large_mib / write_s if write_s else 0.0,
            "read_seconds": read_s,
            "read_mib_per_second": args.large_mib / read_s if read_s else 0.0,
        })

    medians = {
        key: statistics.median(round_[key] for round_ in rounds)
        for key in rounds[0]
    }
    print(json.dumps({
        "files": args.files,
        "round_count": args.rounds,
        "large_mib": args.large_mib,
        "rounds": rounds,
        "median": medians,
    }, indent=2))


if __name__ == "__main__":
    main()
