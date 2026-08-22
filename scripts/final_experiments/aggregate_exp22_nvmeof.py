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

def marker_rows(path, marker):
    rows=[]
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if marker in line: rows.append({k:float(v) for k,v in field_re.findall(line)})
    return rows

def nic_delta(path):
    with path.open(newline="", encoding="utf-8") as f: rows=list(csv.DictReader(f))
    if [r["label"] for r in rows] != ["before","after"]: raise RuntimeError(f"bad NIC CSV: {path}")
    return {k:int(rows[1][k])-int(rows[0][k]) for k in ("compute_rx_bytes","compute_tx_bytes","storage_rx_bytes","storage_tx_bytes")}

def fio_rates(path):
    text=path.read_text(encoding="utf-8", errors="replace")
    # fio writes its SIGINT termination notice before the JSON document when a
    # time-based background job is stopped after the foreground completes.
    doc=json.loads(text[text.find("{"):])
    read_bps=write_bps=0.0
    for job in doc.get("jobs",[]):
        read_bps += float(job.get("read",{}).get("bw_bytes",0))
        write_bps += float(job.get("write",{}).get("bw_bytes",0))
    return read_bps/1e6,write_bps/1e6

per_run=[]; device=[]
for rate in (0,30,60,90):
  for repeat in (1,2,3):
    for system in ("ray-only","ndt-bpe"):
      case=root/f"rate_{rate}"/f"repeat_{repeat}"/system
      if not (case/"COMPLETED").exists(): raise RuntimeError(f"incomplete: {case}")
      bg_read,bg_write=fio_rates(case/"fio.json")
      nic=nic_delta(case/"nic_counters.csv")
      if system=="ray-only":
        files=list((case/"ray_stats").glob("*summary*.json"))
        if len(files)!=1: raise RuntimeError(f"Ray summary count at {case}: {len(files)}")
        result=json.loads(files[0].read_text(encoding="utf-8"))
        wall=float(result["elapsed_s"]); tokens=int(result["total_tokens"]); errors=int(result["total_errors"])
        input_bytes=int(result["total_bytes"]); output_bytes=tokens*4; commands=""
        reads=marker_rows(case/"spdk.log","[NVME_READ_BREAKDOWN]"); writes=marker_rows(case/"spdk.log","[NVME_WRITE_BREAKDOWN]")
        device += [
          {"rate_MBps":rate,"repeat":repeat,"system":system,"operation":"Foreground Device Read","events":len(reads),"bytes":int(sum(x.get("bytes",0) for x in reads)),"service_sum_s":sum(x.get("disk_read_us",0) for x in reads)/1e6},
          {"rate_MBps":rate,"repeat":repeat,"system":system,"operation":"Foreground Device Write","events":len(writes),"bytes":int(sum(x.get("bytes",0) for x in writes)),"service_sum_s":sum(x.get("disk_write_us",0) for x in writes)/1e6},
        ]
      else:
        result=metric_csv(case/"ndt_result.csv")
        wall=float(result["wall_s"]); tokens=int(result["tokens"]); errors=int(result["errors"])
        input_bytes=int(result["input_bytes"]); output_bytes=int(result["output_valid_bytes"]); commands=int(result["commands"])
        fine=marker_rows(case/"spdk.log","[BPE_FINE_BREAKDOWN]"); runtime=marker_rows(case/"runtime.log","[BPE_RUNTIME_BREAKDOWN]"); stage=marker_rows(case/"runtime.log","[BPE_RUNTIME_STAGE]")
        if (len(fine),len(runtime),len(stage))!=(commands,commands,commands): raise RuntimeError(f"bad NDT logs {case}: {len(fine)}/{len(runtime)}/{len(stage)}")
        device += [
          {"rate_MBps":rate,"repeat":repeat,"system":system,"operation":"Foreground Payload Read","events":len(fine),"bytes":int(sum(x.get("payload_bytes",0) for x in fine)),"service_sum_s":sum(x.get("payload_nvme_read_us",0) for x in fine)/1e6},
          {"rate_MBps":rate,"repeat":repeat,"system":system,"operation":"Foreground Payload Write","events":len(fine),"bytes":int(sum(x.get("output_write_bytes",0) for x in fine)),"service_sum_s":sum(x.get("payload_nvme_write_us",0) for x in fine)/1e6},
        ]
      per_run.append({"rate_MBps":rate,"repeat":repeat,"system":system,"wall_s":wall,"throughput_MiB_s":input_bytes/1048576/wall,"actual_bg_read_MBps":bg_read,"actual_bg_write_MBps":bg_write,"input_bytes":input_bytes,"output_bytes":output_bytes,"tokens":tokens,"errors":errors,"commands":commands,**nic,"compute_nic_total_bytes":nic["compute_rx_bytes"]+nic["compute_tx_bytes"]})

with (root/"per_run_summary.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=per_run[0].keys());w.writeheader();w.writerows(per_run)
with (root/"per_run_device_io.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=device[0].keys());w.writeheader();w.writerows(device)

agg=[]
for rate in (0,30,60,90):
  for system in ("ray-only","ndt-bpe"):
    g=[r for r in per_run if r["rate_MBps"]==rate and r["system"]==system]
    row={"rate_MBps":rate,"system":system,"repeats":3}
    for key in ("wall_s","throughput_MiB_s","actual_bg_read_MBps","actual_bg_write_MBps","compute_nic_total_bytes"):
      vals=[float(r[key]) for r in g];row[key+"_mean"]=statistics.mean(vals);row[key+"_stdev"]=statistics.stdev(vals)
    row["tokens_each"]=g[0]["tokens"];row["errors_sum"]=sum(r["errors"] for r in g);agg.append(row)
base={r["system"]:r for r in agg if r["rate_MBps"]==0}
for r in agg:
    r["slowdown_ratio_vs_0"]=r["wall_s_mean"]/base[r["system"]]["wall_s_mean"]
    r["added_penalty_s_vs_0"]=r["wall_s_mean"]-base[r["system"]]["wall_s_mean"]
with (root/"aggregate_summary.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=agg[0].keys());w.writeheader();w.writerows(agg)

trend=[]
for rate in (0,30,60,90):
    ray=next(r for r in agg if r["rate_MBps"]==rate and r["system"]=="ray-only");ndt=next(r for r in agg if r["rate_MBps"]==rate and r["system"]=="ndt-bpe")
    trend.append({"rate_MBps":rate,"ray_wall_s_mean":ray["wall_s_mean"],"ndt_wall_s_mean":ndt["wall_s_mean"],"ray_added_penalty_s":ray["added_penalty_s_vs_0"],"ndt_added_penalty_s":ndt["added_penalty_s_vs_0"],"avoided_contention_penalty_s":ray["added_penalty_s_vs_0"]-ndt["added_penalty_s_vs_0"],"ray_slowdown_pct":(ray["slowdown_ratio_vs_0"]-1)*100,"ndt_slowdown_pct":(ndt["slowdown_ratio_vs_0"]-1)*100,"ray_over_ndt_wall_ratio":ray["wall_s_mean"]/ndt["wall_s_mean"]})
with (root/"trend_summary.csv").open("w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f,fieldnames=trend[0].keys());w.writeheader();w.writerows(trend)
print(json.dumps({"aggregate":agg,"trend":trend},indent=2))
