#!/usr/bin/env python3
import argparse
import csv
import struct
import time
from pathlib import Path

import ndt_compute as ndt


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    args = parser.parse_args()
    run_dir = Path(args.run_dir)
    input_path = Path("/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow")
    index_path = Path("/mnt/nvme/openwebtext_for_ndt_indices/data-00000-of-00080.ndtidx")
    output_path = run_dir / "ndt_output.tokens"

    started = time.perf_counter()
    result = ndt.tokenize_to_nvme(
        dev_path="/dev/ng2n1",
        input_path=str(input_path),
        output_path=str(output_path),
        slots=1,
        fixed_slot=0,
        max_inflight=1,
        queue_depth=256,
        stage_path=str(index_path),
        stage_mode="require-prestaged",
        verbose=False,
    )
    wall_s = time.perf_counter() - started

    manifest = Path(str(output_path) + ".ndtmanifest")
    lines = manifest.read_text(encoding="utf-8").splitlines()
    header = lines[0].split("\t")
    if header[:2] != ["NDTOUT", "2"]:
        raise RuntimeError("unexpected output manifest version")
    pool_bytes, entry_count = int(header[2]), int(header[3])
    valid_sum = 0
    nonzero_samples = 0
    with output_path.open("rb", buffering=0) as output_file:
        for line in lines[1:]:
            _, offset, capacity, valid = map(int, line.split("\t"))
            if valid > capacity:
                raise RuntimeError("valid output exceeds routed capacity")
            valid_sum += valid
            if valid:
                output_file.seek(offset)
                if any(output_file.read(min(valid, 16))):
                    nonzero_samples += 1
    if entry_count != int(result["segments"]):
        raise RuntimeError("manifest entry count does not match command count")
    if valid_sum != int(result["output_valid_bytes"]):
        raise RuntimeError("manifest valid-byte sum mismatch")

    rows = {
        "system": "NDT-BPE",
        "wall_s": wall_s,
        "command_elapsed_s": float(result["elapsed_us"]) / 1_000_000.0,
        "commands": int(result["segments"]),
        "input_bytes": int(result["total_bytes"]),
        "output_pool_bytes": pool_bytes,
        "output_valid_bytes": valid_sum,
        "tokens": valid_sum // struct.calcsize("i"),
        "errors": int(result["errors"]),
        "nonzero_entry_samples": nonzero_samples,
        "stage_cache_hit": bool(result["stage_cache_hit"]),
        "slots": 1,
        "max_inflight": 1,
    }
    for key, value in result.items():
        if key.startswith("breakdown_"):
            rows[key] = value
    with (run_dir / "ndt_result.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["metric", "value"])
        writer.writerows(rows.items())
    print(
        f"NDT_RESULT wall_s={wall_s:.6f} commands={rows['commands']} "
        f"tokens={rows['tokens']} valid_bytes={valid_sum} errors={rows['errors']}"
    )


if __name__ == "__main__":
    main()
