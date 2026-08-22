#!/usr/bin/env python3
import csv
import json
import re
import statistics
import sys
from pathlib import Path


root = Path(sys.argv[1])
field_re = re.compile(r"(?<![A-Za-z0-9_])([A-Za-z0-9_]+)=([0-9.]+)")


def metric_csv(path):
    with path.open(newline="", encoding="utf-8") as f:
        return {r["metric"]: r["value"] for r in csv.DictReader(f)}


def one_csv(path):
    with path.open(newline="", encoding="utf-8") as f:
        return next(csv.DictReader(f))


def markers(path, marker):
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if marker in line:
            out.append({k: float(v) for k, v in field_re.findall(line)})
    return out


def nic_delta(path):
    with path.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if [r["label"] for r in rows] != ["before", "after"]:
        raise RuntimeError(f"bad NIC rows: {path}")
    values = {}
    for key in ("compute_rx_bytes", "compute_tx_bytes", "storage_rx_bytes", "storage_tx_bytes"):
        values[key] = int(rows[1][key]) - int(rows[0][key])
    return values


per_run = []
device_rows = []
for rate in (0, 30, 60, 90):
    for repeat in (1, 2, 3):
        for system in ("ray-only", "ndt-bpe"):
            case = root / f"rate_{rate}" / f"repeat_{repeat}" / system
            if not (case / "COMPLETED").exists():
                raise RuntimeError(f"incomplete case: {case}")
            bg = one_csv(case / "background_client.csv")
            nic = nic_delta(case / "nic_counters.csv")
            if system == "ray-only":
                summaries = list((case / "ray_stats").glob("*summary*.json"))
                if len(summaries) != 1:
                    raise RuntimeError(f"expected one Ray summary: {case}")
                result = json.loads(summaries[0].read_text(encoding="utf-8"))
                wall = float(result["elapsed_s"])
                tokens = int(result["total_tokens"])
                errors = int(result["total_errors"])
                input_bytes = int(result["total_bytes"])
                output_bytes = tokens * 4
                commands = ""
                reads = markers(case / "spdk.log", "[NVME_READ_BREAKDOWN]")
                writes = markers(case / "spdk.log", "[NVME_WRITE_BREAKDOWN]")
                device_rows.extend([
                    {"rate_MBps": rate, "repeat": repeat, "system": system, "operation": "Device Read",
                     "events": len(reads), "bytes": int(sum(r.get("bytes", 0) for r in reads)),
                     "service_sum_s": sum(r.get("disk_read_us", 0) for r in reads) / 1e6},
                    {"rate_MBps": rate, "repeat": repeat, "system": system, "operation": "Device Write",
                     "events": len(writes), "bytes": int(sum(r.get("bytes", 0) for r in writes)),
                     "service_sum_s": sum(r.get("disk_write_us", 0) for r in writes) / 1e6},
                ])
            else:
                result = metric_csv(case / "ndt_result.csv")
                wall = float(result["wall_s"])
                tokens = int(result["tokens"])
                errors = int(result["errors"])
                input_bytes = int(result["input_bytes"])
                output_bytes = int(result["output_valid_bytes"])
                commands = int(result["commands"])
                fine = markers(case / "spdk.log", "[BPE_FINE_BREAKDOWN]")
                runtime = markers(case / "runtime.log", "[BPE_RUNTIME_BREAKDOWN]")
                stage = markers(case / "runtime.log", "[BPE_RUNTIME_STAGE]")
                if (len(fine), len(runtime), len(stage)) != (commands, commands, commands):
                    raise RuntimeError(f"incomplete NDT logs at {case}: {len(fine)}/{len(runtime)}/{len(stage)}")
                device_rows.extend([
                    {"rate_MBps": rate, "repeat": repeat, "system": system, "operation": "Payload Device Read",
                     "events": len(fine), "bytes": int(sum(r.get("payload_bytes", 0) for r in fine)),
                     "service_sum_s": sum(r.get("payload_nvme_read_us", 0) for r in fine) / 1e6},
                    {"rate_MBps": rate, "repeat": repeat, "system": system, "operation": "Payload Device Write",
                     "events": len(fine), "bytes": int(sum(r.get("output_write_bytes", 0) for r in fine)),
                     "service_sum_s": sum(r.get("payload_nvme_write_us", 0) for r in fine) / 1e6},
                ])
            per_run.append({
                "rate_MBps": rate, "repeat": repeat, "system": system, "wall_s": wall,
                "throughput_MiB_s": input_bytes / 1048576 / wall,
                "actual_bg_tx_MBps": float(bg["tx_MBps"]), "actual_bg_rx_MBps": float(bg["rx_MBps"]),
                "input_bytes": input_bytes, "output_bytes": output_bytes, "tokens": tokens,
                "errors": errors, "commands": commands,
                **nic, "compute_nic_total_bytes": nic["compute_rx_bytes"] + nic["compute_tx_bytes"],
            })

with (root / "per_run_summary.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=per_run[0].keys()); w.writeheader(); w.writerows(per_run)
with (root / "per_run_device_io.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=device_rows[0].keys()); w.writeheader(); w.writerows(device_rows)

aggregate = []
for rate in (0, 30, 60, 90):
    for system in ("ray-only", "ndt-bpe"):
        group = [r for r in per_run if r["rate_MBps"] == rate and r["system"] == system]
        row = {"rate_MBps": rate, "system": system, "repeats": len(group)}
        for key in ("wall_s", "throughput_MiB_s", "actual_bg_tx_MBps", "actual_bg_rx_MBps", "compute_nic_total_bytes"):
            values = [float(r[key]) for r in group]
            row[key + "_mean"] = statistics.mean(values)
            row[key + "_stdev"] = statistics.stdev(values)
        row["tokens_each"] = group[0]["tokens"]
        row["errors_sum"] = sum(r["errors"] for r in group)
        aggregate.append(row)

base = {r["system"]: r for r in aggregate if r["rate_MBps"] == 0}
for row in aggregate:
    b = base[row["system"]]["wall_s_mean"]
    row["slowdown_ratio_vs_0"] = row["wall_s_mean"] / b
    row["added_penalty_s_vs_0"] = row["wall_s_mean"] - b
with (root / "aggregate_summary.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=aggregate[0].keys()); w.writeheader(); w.writerows(aggregate)

trend = []
for rate in (0, 30, 60, 90):
    ray = next(r for r in aggregate if r["rate_MBps"] == rate and r["system"] == "ray-only")
    ndt = next(r for r in aggregate if r["rate_MBps"] == rate and r["system"] == "ndt-bpe")
    trend.append({
        "rate_MBps": rate,
        "ray_wall_s_mean": ray["wall_s_mean"], "ndt_wall_s_mean": ndt["wall_s_mean"],
        "ray_added_penalty_s": ray["added_penalty_s_vs_0"], "ndt_added_penalty_s": ndt["added_penalty_s_vs_0"],
        "avoided_saturation_penalty_s": ray["added_penalty_s_vs_0"] - ndt["added_penalty_s_vs_0"],
        "ray_slowdown_pct": (ray["slowdown_ratio_vs_0"] - 1) * 100,
        "ndt_slowdown_pct": (ndt["slowdown_ratio_vs_0"] - 1) * 100,
        "ray_over_ndt_wall_ratio": ray["wall_s_mean"] / ndt["wall_s_mean"],
    })
with (root / "trend_summary.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=trend[0].keys()); w.writeheader(); w.writerows(trend)

print(json.dumps({"aggregate": aggregate, "trend": trend}, indent=2))
