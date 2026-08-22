#!/usr/bin/env python3
import csv
import json
import re
from pathlib import Path

try:
    import matplotlib.pyplot as plt
except ImportError:
    plt = None


ROOT = Path("/mnt/nvme/final_experiments/exp18_writeback_20260818")
RAY_SUMMARY = ROOT / "ray_stats/run_01_summary_exp18_writeback_20260818.json"
FIELD_RE = re.compile(r"(?<![A-Za-z0-9_])([A-Za-z0-9_]+)=([0-9.]+)")


def read_metric_csv(path: Path):
    with path.open(newline="", encoding="utf-8") as stream:
        return {row["metric"]: row["value"] for row in csv.DictReader(stream)}


def parse_marker(path: Path, marker: str):
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if marker not in line:
            continue
        rows.append({key: float(value) for key, value in FIELD_RE.findall(line)})
    return rows


def field_sum(rows, field, scale=1.0):
    return sum(row.get(field, 0.0) for row in rows) * scale


def nic_bytes(path: Path):
    payload = json.loads(path.read_text(encoding="utf-8"))[0]["stats64"]
    return int(payload["rx"]["bytes"]), int(payload["tx"]["bytes"])


ray = json.loads(RAY_SUMMARY.read_text(encoding="utf-8"))
ndt = read_metric_csv(ROOT / "ndt_result.csv")
ray_io_read = parse_marker(ROOT / "ray_spdk.log", "[NVME_READ_BREAKDOWN]")
ray_io_write = parse_marker(ROOT / "ray_spdk.log", "[NVME_WRITE_BREAKDOWN]")
ndt_fine = parse_marker(ROOT / "ndt_spdk.log", "[BPE_FINE_BREAKDOWN]")
ndt_runtime = parse_marker(ROOT / "ndt_runtime.log", "[BPE_RUNTIME_BREAKDOWN]")
ndt_stage = parse_marker(ROOT / "ndt_runtime.log", "[BPE_RUNTIME_STAGE]")

expected_commands = int(ndt["commands"])
if len(ndt_fine) != expected_commands or len(ndt_runtime) != expected_commands or len(ndt_stage) != expected_commands:
    raise RuntimeError(
        f"incomplete NDT logs: fine={len(ndt_fine)} runtime={len(ndt_runtime)} "
        f"stage={len(ndt_stage)} expected={expected_commands}"
    )

ray_elapsed = float(ray["elapsed_s"])
ray_breakdown = ray["breakdown_s"]
ray_phases = {
    "Input access / preprocess": ray_breakdown["arrow_open_s"] + ray_breakdown["text_extract_s"],
    "BPE compute": ray_breakdown["tokenize_s"],
    "Token-output preparation": ray_breakdown["pack_s"],
    "Output write path": ray_breakdown["output_write_s"] + ray_breakdown["fsync_s"],
}
ray_phases["Control / other"] = max(0.0, ray_elapsed - sum(ray_phases.values()))

us_to_s = 1.0 / 1_000_000.0
ndt_elapsed = float(ndt["wall_s"])
ndt_input_path_s = (
    float(ndt["breakdown_segment_build_us"])
    + float(ndt["breakdown_index_preload_us"])
    + field_sum(ndt_fine, "command_parse_us")
    + field_sum(ndt_fine, "payload_read_submit_gap_us")
    + field_sum(ndt_fine, "payload_nvme_read_us")
    + field_sum(ndt_fine, "payload_read_complete_to_dispatch_us")
    + field_sum(ndt_fine, "shm_input_copy_us")
    + field_sum(ndt_fine, "ticket_insert_and_eventfd_us")
    + field_sum(ndt_stage, "input_prepare_us")
) * us_to_s
ndt_bpe_s = field_sum(ndt_stage, "tokenize_us", us_to_s)
ndt_output_shm_s = field_sum(ndt_stage, "output_copy_us", us_to_s)
ndt_writeback_s = (
    field_sum(ndt_fine, "output_copy_us")
    + field_sum(ndt_fine, "payload_nvme_write_us")
) * us_to_s
ndt_phases = {
    "Input access / preprocess": ndt_input_path_s,
    "BPE compute": ndt_bpe_s,
    "Token-output preparation": ndt_output_shm_s,
    "Output write path": ndt_writeback_s,
}
ndt_phases["Control / other"] = max(0.0, ndt_elapsed - sum(ndt_phases.values()))

systems = [("Ray-only", ray_elapsed, ray_phases), ("NDT-BPE", ndt_elapsed, ndt_phases)]
with (ROOT / "breakdown_wall.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.writer(stream)
    writer.writerow(["system", "phase", "seconds", "percent_of_wall"])
    for system, elapsed, phases in systems:
        for phase, seconds in phases.items():
            writer.writerow([system, phase, f"{seconds:.6f}", f"{seconds / elapsed * 100:.6f}"])

ray_rx0, ray_tx0 = nic_bytes(ROOT / "ray_nic_before.json")
ray_rx1, ray_tx1 = nic_bytes(ROOT / "ray_nic_after.json")
ndt_rx0, ndt_tx0 = nic_bytes(ROOT / "ndt_nic_before.json")
ndt_rx1, ndt_tx1 = nic_bytes(ROOT / "ndt_nic_after.json")

io_rows = [
    ["Ray-only", "Device Read", len(ray_io_read), int(field_sum(ray_io_read, "bytes")), field_sum(ray_io_read, "disk_read_us", us_to_s)],
    ["Ray-only", "Device Write", len(ray_io_write), int(field_sum(ray_io_write, "bytes")), field_sum(ray_io_write, "disk_write_us", us_to_s)],
    ["NDT-BPE", "Payload Device Read", len(ndt_fine), int(field_sum(ndt_fine, "payload_bytes")), field_sum(ndt_fine, "payload_nvme_read_us", us_to_s)],
    ["NDT-BPE", "Payload Device Write", len(ndt_fine), int(field_sum(ndt_fine, "output_write_bytes")), field_sum(ndt_fine, "payload_nvme_write_us", us_to_s)],
]
with (ROOT / "device_io_service.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.writer(stream)
    writer.writerow(["system", "operation", "events", "bytes", "service_sum_s"])
    writer.writerows(io_rows)

summary_rows = [
    ["Ray-only", ray_elapsed, int(ray["total_bytes"]), int(ray["total_tokens"]), int(ray["total_errors"]), ray_rx1 - ray_rx0, ray_tx1 - ray_tx0],
    ["NDT-BPE", ndt_elapsed, int(ndt["input_logical_bytes"]), int(ndt["tokens"]), int(ndt["errors"]), ndt_rx1 - ndt_rx0, ndt_tx1 - ndt_tx0],
]
with (ROOT / "run_summary.csv").open("w", newline="", encoding="utf-8") as stream:
    writer = csv.writer(stream)
    writer.writerow(["system", "wall_s", "input_bytes", "tokens", "errors", "compute_nic_rx_bytes", "compute_nic_tx_bytes"])
    writer.writerows(summary_rows)

colors = {
    "Input access / preprocess": "#577590",
    "BPE compute": "#E76F51",
    "Token-output preparation": "#F2CC8F",
    "Output write path": "#2A9D8F",
    "Control / other": "#A7A7A7",
}
if plt is not None:
    fig, ax = plt.subplots(figsize=(7.2, 5.0), dpi=200)
    bottoms = [0.0, 0.0]
    for phase in colors:
        values = [ray_phases[phase], ndt_phases[phase]]
        ax.bar([0, 1], values, bottom=bottoms, width=0.58, label=phase,
               color=colors[phase], edgecolor="black", linewidth=0.5)
        bottoms = [bottoms[i] + values[i] for i in range(2)]
    for index, elapsed in enumerate([ray_elapsed, ndt_elapsed]):
        ax.text(index, elapsed + 2.0, f"{elapsed:.2f} s", ha="center", va="bottom", fontsize=10, fontweight="bold")
    ax.set_xticks([0, 1], ["Ray-only\n1 worker", "NDT-BPE\n1 slot"])
    ax.set_ylabel("Wall-clock time (s)")
    ax.set_title("Single-Shard End-to-End Breakdown (Write-Back Enabled)")
    ax.grid(axis="y", alpha=0.25, linewidth=0.6)
    ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1.0), frameon=False, fontsize=8)
    ax.set_ylim(0, max(ray_elapsed, ndt_elapsed) * 1.13)
    fig.tight_layout()
    fig.savefig(ROOT / "exp18_single_shard_breakdown_writeback.png", bbox_inches="tight")
    fig.savefig(ROOT / "exp18_single_shard_breakdown_writeback.pdf", bbox_inches="tight")
    plt.close(fig)

print(f"ray_wall_s={ray_elapsed:.6f}")
print(f"ndt_wall_s={ndt_elapsed:.6f}")
print(f"ray_tokens={ray['total_tokens']} ndt_tokens={ndt['tokens']}")
print(f"ray_nic_total={ray_rx1 - ray_rx0 + ray_tx1 - ray_tx0}")
print(f"ndt_nic_total={ndt_rx1 - ndt_rx0 + ndt_tx1 - ndt_tx0}")
print(f"ndt_fine={len(ndt_fine)} ndt_runtime={len(ndt_runtime)} ndt_stage={len(ndt_stage)}")
