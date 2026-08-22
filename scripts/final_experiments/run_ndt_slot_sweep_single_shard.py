#!/usr/bin/env python3
import argparse
import csv
import struct
import time
from pathlib import Path

import ndt_compute as ndt

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--run-dir",required=True)
    p.add_argument("--slots",required=True,type=int)
    p.add_argument("--runtime-workers",required=True,type=int)
    args=p.parse_args()
    run_dir=Path(args.run_dir)
    input_path=Path("/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow")
    index_path=Path("/mnt/nvme/openwebtext_for_ndt_indices/data-00000-of-00080.ndtidx")
    output_path=run_dir/"ndt_output.tokens"
    started=time.perf_counter()
    result=ndt.tokenize_to_nvme(
        dev_path="/dev/ng2n1", input_path=str(input_path), output_path=str(output_path),
        slots=args.slots, fixed_slot=-1, max_inflight=args.slots, queue_depth=256,
        stage_path=str(index_path), stage_mode="require-prestaged", verbose=False)
    wall_s=time.perf_counter()-started
    manifest=Path(str(output_path)+".ndtmanifest")
    lines=manifest.read_text(encoding="utf-8").splitlines(); header=lines[0].split("\t")
    if header[:2] != ["NDTOUT","2"]: raise RuntimeError("unexpected manifest")
    valid_sum=0; nonzero=0
    with output_path.open("rb",buffering=0) as f:
        for line in lines[1:]:
            _,offset,capacity,valid=map(int,line.split("\t"))
            if valid>capacity: raise RuntimeError("valid exceeds capacity")
            valid_sum+=valid
            if valid:
                f.seek(offset)
                if any(f.read(min(valid,16))): nonzero+=1
    if len(lines)-1 != int(result["segments"]): raise RuntimeError("command count mismatch")
    if valid_sum != int(result["output_valid_bytes"]): raise RuntimeError("valid byte mismatch")
    row={"slots":args.slots,"max_inflight":args.slots,"runtime_workers":args.runtime_workers,"wall_s":wall_s,
         "command_elapsed_s":float(result["elapsed_us"])/1e6,"commands":int(result["segments"]),
         "input_bytes":int(result["total_bytes"]),"output_valid_bytes":valid_sum,
         "tokens":valid_sum//struct.calcsize("i"),"errors":int(result["errors"]),
         "nonzero_entry_samples":nonzero,"stage_cache_hit":bool(result["stage_cache_hit"])}
    for k,v in result.items():
        if k.startswith("breakdown_"): row[k]=v
    with (run_dir/"ndt_result.csv").open("w",newline="",encoding="utf-8") as f:
        w=csv.writer(f);w.writerow(["metric","value"]);w.writerows(row.items())
    print(f"SLOT_RESULT runtime_workers={args.runtime_workers} slots={args.slots} wall_s={wall_s:.6f} commands={row['commands']} tokens={row['tokens']} errors={row['errors']}")

if __name__=="__main__": main()
