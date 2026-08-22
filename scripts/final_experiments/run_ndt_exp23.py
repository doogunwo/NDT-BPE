#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import os
import struct
import time
from pathlib import Path

import ndt_compute as nc


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--index-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--stats-dir", required=True, type=Path)
    parser.add_argument("--condition", required=True)
    parser.add_argument("--run", required=True, type=int)
    parser.add_argument("--slots", required=True, type=int)
    parser.add_argument("--max-inflight", required=True, type=int)
    parser.add_argument("--runtime-workers", required=True, type=int)
    parser.add_argument("--limit", type=int, default=80)
    parser.add_argument("--cold-cache", action="store_true")
    args = parser.parse_args()

    inputs = sorted(args.input_dir.glob("data-*-of-00080.arrow"))[: args.limit]
    if len(inputs) != args.limit:
        raise SystemExit(f"expected {args.limit} shards, found {len(inputs)}")
    if args.cold_cache:
        os.sync()
        Path("/proc/sys/vm/drop_caches").write_text("3\n", encoding="ascii")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.stats_dir.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, object]] = []
    started = time.perf_counter()

    for ordinal, input_path in enumerate(inputs, 1):
        stem = input_path.stem
        index_path = args.index_dir / f"{stem}.ndtidx"
        output_path = args.output_dir / f"{stem}.bin"
        if not output_path.exists():
            raise RuntimeError(f"preallocated write-back file is missing: {output_path}")
        if not index_path.exists():
            raise RuntimeError(f"prestaged index is missing: {index_path}")
        shard_started = time.perf_counter()
        result = nc.tokenize_to_nvme(
            dev_path=args.device,
            input_path=str(input_path),
            output_path=str(output_path),
            queue_depth=64,
            max_inflight=args.max_inflight,
            slots=args.slots,
            fixed_slot=0 if args.slots == 1 else -1,
            stage_path=str(index_path),
            stage_mode="require-prestaged",
        )
        shard_wall = time.perf_counter() - shard_started
        output_valid_bytes = int(result.get("output_valid_bytes", 0))
        if output_valid_bytes <= 0 or output_valid_bytes % struct.calcsize("i") != 0:
            raise RuntimeError(f"invalid output byte count for {input_path}: {output_valid_bytes}")
        row = {
            "ordinal": ordinal,
            "input": str(input_path),
            "input_bytes": int(result["total_bytes"]),
            "output_valid_bytes": output_valid_bytes,
            "tokens": output_valid_bytes // struct.calcsize("i"),
            "errors": int(result["errors"]),
            "segments": int(result["segments"]),
            "wall_s": shard_wall,
            "device_phase_s": float(result["elapsed_us"]) / 1_000_000,
            "cqe_wait_s": float(result.get("breakdown_cqe_wait_us", 0)) / 1_000_000,
        }
        for key, value in result.items():
            if key.startswith("breakdown_") and key != "breakdown_cqe_wait_us":
                row[key] = value
        rows.append(row)
        print(
            f"SHARD_DONE ordinal={ordinal}/{len(inputs)} file={input_path.name} "
            f"wall_s={shard_wall:.6f} errors={row['errors']}",
            flush=True,
        )
        if row["errors"]:
            raise RuntimeError(f"NDT errors in {input_path}: {row['errors']}")

    elapsed_s = time.perf_counter() - started
    per_file = args.stats_dir / f"run_{args.run:02d}_per_file.csv"
    with per_file.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

    total_bytes = sum(int(row["input_bytes"]) for row in rows)
    summary = {
        "system": "NDT-BPE",
        "condition": args.condition,
        "run": args.run,
        "runtime_workers": args.runtime_workers,
        "slots": args.slots,
        "max_inflight": args.max_inflight,
        "num_files": len(rows),
        "total_bytes": total_bytes,
        "total_tokens": sum(int(row["tokens"]) for row in rows),
        "total_errors": sum(int(row["errors"]) for row in rows),
        "elapsed_s": elapsed_s,
        "throughput_MiBps": total_bytes / 1048576 / elapsed_s,
        "sum_device_phase_s": sum(float(row["device_phase_s"]) for row in rows),
        "sum_cqe_wait_s": sum(float(row["cqe_wait_s"]) for row in rows),
        "device": args.device,
        "per_file_csv": str(per_file),
    }
    summary_path = args.stats_dir / f"run_{args.run:02d}_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("SUMMARY " + json.dumps(summary, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
