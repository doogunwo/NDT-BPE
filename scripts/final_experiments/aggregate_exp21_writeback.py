#!/usr/bin/env python3
import csv
import json
import math
import re
import statistics
import sys
from pathlib import Path


ROOT = Path(sys.argv[1])
FIELD_RE = re.compile(r"(?<![A-Za-z0-9_])([A-Za-z0-9_]+)=([0-9.]+)")


def metric_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return {row["metric"]: row["value"] for row in csv.DictReader(stream)}


def marker(path, name):
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if name in line:
            rows.append({k: float(v) for k, v in FIELD_RE.findall(line)})
    return rows


def nic(path):
    stats = json.loads(path.read_text(encoding="utf-8"))[0]["stats64"]
    return int(stats["rx"]["bytes"]), int(stats["tx"]["bytes"])


def find_ray_summary(run):
    found = list((run / "ray_stats").glob("*summary*.json"))
    if len(found) != 1:
        raise RuntimeError(f"expected one Ray summary in {run}, got {found}")
    return json.loads(found[0].read_text(encoding="utf-8"))


rows = []
phase_rows = []
device_rows = []
for number in range(1, 4):
    run = ROOT / f"run_{number:02d}"
    ray = find_ray_summary(run)
    ndt = metric_csv(run / "ndt_result.csv")
    fine = marker(run / "ndt_spdk.log", "[BPE_FINE_BREAKDOWN]")
    runtime = marker(run / "ndt_runtime.log", "[BPE_RUNTIME_BREAKDOWN]")
    stage = marker(run / "ndt_runtime.log", "[BPE_RUNTIME_STAGE]")
    ray_reads = marker(run / "ray_spdk.log", "[NVME_READ_BREAKDOWN]")
    ray_writes = marker(run / "ray_spdk.log", "[NVME_WRITE_BREAKDOWN]")
    commands = int(ndt["commands"])
    if (len(fine), len(runtime), len(stage)) != (commands, commands, commands):
        raise RuntimeError(f"run {number}: incomplete NDT log {len(fine)}/{len(runtime)}/{len(stage)} expected {commands}")
    for system, record, before, after in [
        ("Ray-only", ray, run / "ray_nic_before.json", run / "ray_nic_after.json"),
        ("NDT-BPE", ndt, run / "ndt_nic_before.json", run / "ndt_nic_after.json"),
    ]:
        rx0, tx0 = nic(before); rx1, tx1 = nic(after)
        wall = float(record["elapsed_s"] if system == "Ray-only" else record["wall_s"])
        input_bytes = int(record["total_bytes"] if system == "Ray-only" else record["input_bytes"])
        output_bytes = int(ray["total_tokens"]) * 4 if system == "Ray-only" else int(record["output_valid_bytes"])
        tokens = int(record["total_tokens"] if system == "Ray-only" else record["tokens"])
        errors = int(record["total_errors"] if system == "Ray-only" else record["errors"])
        rows.append({"run": number, "system": system, "wall_s": wall,
                     "throughput_MiB_s": input_bytes / 1048576 / wall,
                     "input_bytes": input_bytes, "output_bytes": output_bytes,
                     "tokens": tokens, "errors": errors,
                     "compute_nic_rx_bytes": rx1-rx0, "compute_nic_tx_bytes": tx1-tx0,
                     "compute_nic_total_bytes": rx1-rx0+tx1-tx0,
                     "commands": "" if system == "Ray-only" else commands})
    rb = ray["breakdown_s"]
    ray_phase = {
        "Input access / preprocess": rb["arrow_open_s"] + rb["text_extract_s"],
        "BPE compute": rb["tokenize_s"],
        "Token-output preparation": rb["pack_s"],
        "Output write path": rb["output_write_s"] + rb["fsync_s"],
    }
    ray_phase["Control / other"] = max(0.0, float(ray["elapsed_s"]) - sum(ray_phase.values()))
    us = 1e-6
    field_sum = lambda source, key: sum(item.get(key, 0.0) for item in source)
    device_rows.extend([
        {"run": number, "system": "Ray-only", "operation": "Device Read", "events": len(ray_reads),
         "bytes": int(field_sum(ray_reads, "bytes")), "service_sum_s": field_sum(ray_reads, "disk_read_us") * us},
        {"run": number, "system": "Ray-only", "operation": "Device Write", "events": len(ray_writes),
         "bytes": int(field_sum(ray_writes, "bytes")), "service_sum_s": field_sum(ray_writes, "disk_write_us") * us},
        {"run": number, "system": "NDT-BPE", "operation": "Payload Device Read", "events": len(fine),
         "bytes": int(field_sum(fine, "payload_bytes")), "service_sum_s": field_sum(fine, "payload_nvme_read_us") * us},
        {"run": number, "system": "NDT-BPE", "operation": "Payload Device Write", "events": len(fine),
         "bytes": int(field_sum(fine, "output_write_bytes")), "service_sum_s": field_sum(fine, "payload_nvme_write_us") * us},
    ])
    ndt_phase = {
        "Input access / preprocess": (float(ndt["breakdown_segment_build_us"]) + float(ndt["breakdown_index_preload_us"]) +
            field_sum(fine, "command_parse_us") + field_sum(fine, "payload_read_submit_gap_us") +
            field_sum(fine, "payload_nvme_read_us") + field_sum(fine, "payload_read_complete_to_dispatch_us") +
            field_sum(fine, "shm_input_copy_us") + field_sum(fine, "ticket_insert_and_eventfd_us") +
            field_sum(stage, "input_prepare_us")) * us,
        "BPE compute": field_sum(stage, "tokenize_us") * us,
        "Token-output preparation": field_sum(stage, "output_copy_us") * us,
        "Output write path": (field_sum(fine, "output_copy_us") + field_sum(fine, "payload_nvme_write_us")) * us,
    }
    ndt_phase["Control / other"] = max(0.0, float(ndt["wall_s"]) - sum(ndt_phase.values()))
    for system, phases in [("Ray-only", ray_phase), ("NDT-BPE", ndt_phase)]:
        for phase, seconds in phases.items():
            phase_rows.append({"run": number, "system": system, "phase": phase, "seconds": seconds})

with (ROOT / "per_run_summary.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
with (ROOT / "per_run_breakdown.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=phase_rows[0].keys()); writer.writeheader(); writer.writerows(phase_rows)
with (ROOT / "per_run_device_io.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=device_rows[0].keys()); writer.writeheader(); writer.writerows(device_rows)

aggregate = []
for system in ("Ray-only", "NDT-BPE"):
    group = [r for r in rows if r["system"] == system]
    item = {"system": system, "repeats": len(group)}
    for field in ("wall_s", "throughput_MiB_s", "compute_nic_total_bytes"):
        values = [float(r[field]) for r in group]
        item[field + "_mean"] = statistics.mean(values)
        item[field + "_stdev"] = statistics.stdev(values)
    item["tokens_each"] = group[0]["tokens"]
    item["errors_sum"] = sum(r["errors"] for r in group)
    aggregate.append(item)
with (ROOT / "aggregate_summary.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=aggregate[0].keys()); writer.writeheader(); writer.writerows(aggregate)

phase_aggregate = []
for system in ("Ray-only", "NDT-BPE"):
    for phase in ("Input access / preprocess", "BPE compute", "Token-output preparation", "Output write path", "Control / other"):
        values = [r["seconds"] for r in phase_rows if r["system"] == system and r["phase"] == phase]
        phase_aggregate.append({"system": system, "phase": phase,
                                "seconds_mean": statistics.mean(values), "seconds_stdev": statistics.stdev(values)})
with (ROOT / "aggregate_breakdown.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=phase_aggregate[0].keys()); writer.writeheader(); writer.writerows(phase_aggregate)

device_aggregate = []
for system, operation in (("Ray-only", "Device Read"), ("Ray-only", "Device Write"),
                          ("NDT-BPE", "Payload Device Read"), ("NDT-BPE", "Payload Device Write")):
    group = [r for r in device_rows if r["system"] == system and r["operation"] == operation]
    device_aggregate.append({
        "system": system, "operation": operation,
        "events_mean": statistics.mean(r["events"] for r in group),
        "bytes_mean": statistics.mean(r["bytes"] for r in group),
        "service_sum_s_mean": statistics.mean(r["service_sum_s"] for r in group),
        "service_sum_s_stdev": statistics.stdev(r["service_sum_s"] for r in group),
    })
with (ROOT / "aggregate_device_io.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.DictWriter(stream, fieldnames=device_aggregate[0].keys()); writer.writeheader(); writer.writerows(device_aggregate)

ray_a, ndt_a = aggregate
comparison = {
    "ray_wall_s_mean": ray_a["wall_s_mean"], "ndt_wall_s_mean": ndt_a["wall_s_mean"],
    "ndt_over_ray_wall_ratio": ndt_a["wall_s_mean"] / ray_a["wall_s_mean"],
    "compute_nic_reduction_pct": (1.0 - ndt_a["compute_nic_total_bytes_mean"] / ray_a["compute_nic_total_bytes_mean"]) * 100.0,
    "output_write_back_enabled": True,
}
(ROOT / "comparison.json").write_text(json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"aggregate": aggregate, "comparison": comparison}, indent=2))
