#!/usr/bin/env python3
import csv
import os
import struct
import time
from pathlib import Path

import ndt_compute as ndt


INPUT = Path("/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow")
INDEX = Path("/mnt/nvme/openwebtext_for_ndt_indices/data-00000-of-00080.ndtidx")
OUTPUT = Path("/mnt/nvme/final_experiments/exp18_writeback_20260818/ndt_output/shard0.tokens")
RESULT = Path("/mnt/nvme/final_experiments/exp18_writeback_20260818/ndt_result.csv")


started = time.perf_counter()
result = ndt.tokenize_to_nvme(
    dev_path="/dev/ng2n1",
    input_path=str(INPUT),
    output_path=str(OUTPUT),
    slots=1,
    fixed_slot=0,
    max_inflight=1,
    queue_depth=256,
    stage_path=str(INDEX),
    stage_mode="require-prestaged",
    verbose=False,
)
wall_s = time.perf_counter() - started

manifest = Path(str(OUTPUT) + ".ndtmanifest")
lines = manifest.read_text(encoding="utf-8").splitlines()
header = lines[0].split("\t")
if header[:2] != ["NDTOUT", "2"]:
    raise RuntimeError("unexpected output manifest version")

pool_bytes = int(header[2])
entry_count = int(header[3])
valid_sum = 0
nonzero_samples = 0
with OUTPUT.open("rb", buffering=0) as output_file:
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
    raise RuntimeError("manifest valid-byte sum does not match completion result")

rows = {
    "system": "ndt-bpe",
    "input_path": str(INPUT),
    "index_path": str(INDEX),
    "output_path": str(OUTPUT),
    "slots": 1,
    "max_inflight": 1,
    "wall_s": wall_s,
    "command_elapsed_s": float(result["elapsed_us"]) / 1_000_000.0,
    "commands": int(result["segments"]),
    "input_logical_bytes": int(result["total_bytes"]),
    "output_pool_bytes": pool_bytes,
    "output_valid_bytes": valid_sum,
    "tokens": valid_sum // struct.calcsize("i"),
    "errors": int(result["errors"]),
    "nonzero_entry_samples": nonzero_samples,
    "stage_cache_hit": bool(result["stage_cache_hit"]),
}
for key, value in result.items():
    if key.startswith("breakdown_"):
        rows[key] = value

RESULT.parent.mkdir(parents=True, exist_ok=True)
with RESULT.open("w", newline="", encoding="utf-8") as stream:
    writer = csv.writer(stream)
    writer.writerow(["metric", "value"])
    writer.writerows(rows.items())

print(
    f"NDT_RESULT wall_s={wall_s:.6f} commands={rows['commands']} "
    f"tokens={rows['tokens']} valid_bytes={valid_sum} errors={rows['errors']}"
)
