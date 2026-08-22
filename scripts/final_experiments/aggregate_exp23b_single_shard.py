#!/usr/bin/env python3
import csv
import json
import statistics
import sys
from pathlib import Path

root=Path(sys.argv[1])

def metrics(path):
    with path.open(newline="",encoding="utf-8") as f:
        return {r["metric"]:r["value"] for r in csv.DictReader(f)}

def nic(path):
    return json.loads(path.read_text(encoding="utf-8"))

rows=[]
for slots in (1,2,4,8,16):
  for repeat in (1,2,3):
    d=root/f"slots_{slots}"/f"repeat_{repeat}"
    if not (d/"COMPLETED").exists(): raise RuntimeError(f"incomplete case: {d}")
    m=metrics(d/"ndt_result.csv")
    commands=int(m["commands"])
    fine=sum("[BPE_FINE_BREAKDOWN]" in x for x in (d/"spdk.log").read_text(errors="replace").splitlines())
    runtime=sum("[BPE_RUNTIME_BREAKDOWN]" in x for x in (d/"runtime.log").read_text(errors="replace").splitlines())
    stage=sum("[BPE_RUNTIME_STAGE]" in x for x in (d/"runtime.log").read_text(errors="replace").splitlines())
    if (fine,runtime,stage)!=(commands,commands,commands): raise RuntimeError(f"bad logs {d}: {fine}/{runtime}/{stage}")
    before=nic(d/"nic_before.json");after=nic(d/"nic_after.json")
    wall=float(m["wall_s"]);input_bytes=int(m["input_bytes"])
    rows.append({"slots":slots,"max_inflight":slots,"runtime_workers":2,"repeat":repeat,
      "wall_s":wall,"throughput_MiB_s":input_bytes/1048576/wall,"input_bytes":input_bytes,
      "output_valid_bytes":int(m["output_valid_bytes"]),"tokens":int(m["tokens"]),
      "errors":int(m["errors"]),"commands":commands,"nonzero_entry_samples":int(m["nonzero_entry_samples"]),
      "fine_records":fine,"runtime_records":runtime,"stage_records":stage,
      "compute_nic_rx_bytes":after["rx_bytes"]-before["rx_bytes"],
      "compute_nic_tx_bytes":after["tx_bytes"]-before["tx_bytes"]})
with (root/"per_run_summary.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)

base=statistics.mean(r["wall_s"] for r in rows if r["slots"]==1)
agg=[]
for slots in (1,2,4,8,16):
    g=[r for r in rows if r["slots"]==slots]
    wall=[r["wall_s"] for r in g];tp=[r["throughput_MiB_s"] for r in g]
    mean=statistics.mean(wall)
    agg.append({"slots":slots,"max_inflight":slots,"runtime_workers":2,"repeats":3,
      "wall_s_mean":mean,"wall_s_stdev":statistics.stdev(wall),
      "throughput_MiB_s_mean":statistics.mean(tp),"throughput_MiB_s_stdev":statistics.stdev(tp),
      "speedup_vs_slot1":base/mean,"wall_reduction_pct_vs_slot1":(base-mean)/base*100,
      "tokens_each":g[0]["tokens"],"errors_sum":sum(r["errors"] for r in g)})
with (root/"aggregate_summary.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=agg[0].keys());w.writeheader();w.writerows(agg)

ray_wall=67.9827053360641;ray_tp=7.060109383562084
comparison=[]
for r in agg:
    comparison.append({"slots":r["slots"],"ndt_wall_s_mean":r["wall_s_mean"],"ray_1worker_wall_s_mean":ray_wall,
      "ndt_over_ray_wall_ratio":r["wall_s_mean"]/ray_wall,"ndt_wall_delta_pct_vs_ray":(r["wall_s_mean"]/ray_wall-1)*100,
      "ndt_throughput_MiB_s":r["throughput_MiB_s_mean"],"ray_throughput_MiB_s":ray_tp,
      "ndt_throughput_delta_pct_vs_ray":(r["throughput_MiB_s_mean"]/ray_tp-1)*100})
with (root/"ray_comparison.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=comparison[0].keys());w.writeheader();w.writerows(comparison)
print(json.dumps({"aggregate":agg,"ray_comparison":comparison},indent=2))
