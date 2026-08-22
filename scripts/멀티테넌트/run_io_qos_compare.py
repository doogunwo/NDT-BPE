#!/usr/bin/env python3
"""
멀티테넌트 환경에서 I/O alone 과 I/O + NDP 조건을 비교하는 스크립트.
"""

import argparse
import json
import multiprocessing as mp
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
COMPUTE_DIR = REPO_ROOT / "compute"
if str(COMPUTE_DIR) not in sys.path:
    sys.path.insert(0, str(COMPUTE_DIR))

import ndt_compute as nc


RESULT_DIR_PREFIX = "실험결과_"


def parse_slots(value: str) -> List[int]:
    slots: List[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        slot = int(item)
        if slot < 1:
            raise ValueError("ndp slot 수는 1 이상이어야 합니다.")
        slots.append(slot)
    if not slots:
        raise ValueError("ndp slot 목록이 비어 있습니다.")
    return slots


def next_result_dir(root: Path) -> Path:
    idx = 1
    while True:
        candidate = root / f"{RESULT_DIR_PREFIX}{idx}"
        if not candidate.exists():
            return candidate
        idx += 1


def list_arrow_paths(input_dir: Path, pattern: str) -> List[Path]:
    return sorted(path for path in input_dir.rglob(pattern) if path.is_file())


def drop_linux_caches() -> None:
    if os.geteuid() != 0:
        raise SystemExit("cold-cache 실행은 root 권한이 필요합니다.")
    subprocess.run(["sync"], check=True)
    Path("/proc/sys/vm/drop_caches").write_text("3\n", encoding="utf-8")


def parse_fio_json(path: Path) -> Dict[str, float]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    jobs = payload.get("jobs", [])
    read_bw = 0.0
    write_bw = 0.0
    read_iops = 0.0
    write_iops = 0.0
    read_p95 = 0.0
    read_p99 = 0.0
    write_p95 = 0.0
    write_p99 = 0.0

    for job in jobs:
        read = job.get("read", {})
        write = job.get("write", {})
        read_bw += float(read.get("bw_bytes", 0.0))
        write_bw += float(write.get("bw_bytes", 0.0))
        read_iops += float(read.get("iops", 0.0))
        write_iops += float(write.get("iops", 0.0))
        read_pct = read.get("clat_ns", {}).get("percentile", {})
        write_pct = write.get("clat_ns", {}).get("percentile", {})
        read_p95 = max(read_p95, float(read_pct.get("95.000000", 0.0)) / 1000.0)
        read_p99 = max(read_p99, float(read_pct.get("99.000000", 0.0)) / 1000.0)
        write_p95 = max(write_p95, float(write_pct.get("95.000000", 0.0)) / 1000.0)
        write_p99 = max(write_p99, float(write_pct.get("99.000000", 0.0)) / 1000.0)

    return {
        "read_bw_MBps": read_bw / (1024.0 * 1024.0),
        "write_bw_MBps": write_bw / (1024.0 * 1024.0),
        "total_bw_MBps": (read_bw + write_bw) / (1024.0 * 1024.0),
        "read_iops": read_iops,
        "write_iops": write_iops,
        "total_iops": read_iops + write_iops,
        "read_p95_us": read_p95,
        "read_p99_us": read_p99,
        "write_p95_us": write_p95,
        "write_p99_us": write_p99,
    }


def run_fio(fio_bin: str, fio_job: Path, output_json: Path, output_log: Path) -> Dict[str, float]:
    cmd = [fio_bin, "--output-format=json", f"--output={output_json}", str(fio_job)]
    start = time.perf_counter()
    with output_log.open("w", encoding="utf-8") as f:
        f.write("$ " + " ".join(cmd) + "\n")
        f.flush()
        proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, text=True)
        rc = proc.wait()
    elapsed_s = time.perf_counter() - start
    if rc != 0:
        raise RuntimeError(f"fio failed rc={rc}")
    summary = parse_fio_json(output_json)
    summary["elapsed_s"] = elapsed_s
    return summary


def ndp_load_worker(
    stop_event: mp.Event,
    input_paths: List[str],
    output_dir: str,
    dev_path: str,
    slots: int,
    queue_depth: int,
    max_inflight: int,
    opcode: int,
    nsid: int,
    admin: bool,
    verbose: bool,
    stats_path: str,
) -> None:
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    total_bytes = 0
    total_files = 0
    total_loops = 0
    total_errors = 0
    start = time.perf_counter()

    while not stop_event.is_set():
        total_loops += 1
        for input_path in input_paths:
            if stop_event.is_set():
                break
            path = Path(input_path)
            out_path = out_dir / f"{path.stem}.bin"
            result = nc.tokenize_to_nvme(
                dev_path=dev_path,
                input_path=str(path),
                output_path=str(out_path),
                opcode=opcode,
                nsid=nsid,
                queue_depth=queue_depth,
                max_inflight=max_inflight,
                slots=slots,
                admin=admin,
                verbose=verbose,
            )
            total_files += 1
            total_bytes += int(result["total_bytes"])
            total_errors += int(result["errors"])

    elapsed_s = time.perf_counter() - start
    payload = {
        "loops": total_loops,
        "files_processed": total_files,
        "total_bytes": total_bytes,
        "total_errors": total_errors,
        "elapsed_s": elapsed_s,
        "throughput_MBps": ((total_bytes / (1024.0 * 1024.0)) / elapsed_s) if elapsed_s > 0 else 0.0,
        "slots": slots,
    }
    Path(stats_path).write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")


def run_io_alone(fio_bin: str, fio_job: Path, run_dir: Path) -> Dict[str, float]:
    fio_json = run_dir / "fio.json"
    fio_log = run_dir / "fio.log"
    summary = run_fio(fio_bin, fio_job, fio_json, fio_log)
    (run_dir / "summary.json").write_text(
        json.dumps({"scenario": "io_alone", "fio": summary}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return summary


def run_io_plus_ndp(
    fio_bin: str,
    fio_job: Path,
    run_dir: Path,
    input_paths: List[Path],
    ndp_output_dir: Path,
    dev_path: str,
    ndp_slots: int,
    queue_depth: int,
    max_inflight: int,
    opcode: int,
    nsid: int,
    admin: bool,
    verbose: bool,
    stop_timeout_s: float,
) -> Dict[str, object]:
    fio_json = run_dir / "fio.json"
    fio_log = run_dir / "fio.log"
    ndp_stats_path = run_dir / "ndp_summary.json"
    stop_event = mp.Event()
    ndp_proc = mp.Process(
        target=ndp_load_worker,
        args=(
            stop_event,
            [str(path) for path in input_paths],
            str(ndp_output_dir),
            dev_path,
            ndp_slots,
            queue_depth,
            max_inflight,
            opcode,
            nsid,
            admin,
            verbose,
            str(ndp_stats_path),
        ),
    )
    ndp_proc.start()
    try:
        fio_summary = run_fio(fio_bin, fio_job, fio_json, fio_log)
    finally:
        stop_event.set()
        if stop_timeout_s > 0:
            ndp_proc.join(timeout=stop_timeout_s)
            if ndp_proc.is_alive():
                ndp_proc.kill()
                ndp_proc.join()
        else:
            # 기본 동작은 현재 처리 중인 샤드가 끝날 때까지 기다려 정상 종료를 보장한다.
            ndp_proc.join()

    ndp_summary = {}
    if ndp_stats_path.exists():
        ndp_summary = json.loads(ndp_stats_path.read_text(encoding="utf-8"))

    summary = {"scenario": "io_plus_ndp", "fio": fio_summary, "ndp": ndp_summary}
    (run_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return summary


def compute_degradation(alone: Dict[str, float], shared: Dict[str, float]) -> Dict[str, float]:
    def ratio(shared_key: str, alone_key: str, invert: bool = False) -> float:
        base = float(alone.get(alone_key, 0.0))
        cur = float(shared.get(shared_key, 0.0))
        if base == 0.0:
            return 0.0
        if invert:
            return cur / base
        return cur / base

    return {
        "bw_ratio": ratio("total_bw_MBps", "total_bw_MBps"),
        "iops_ratio": ratio("total_iops", "total_iops"),
        "read_p99_inflation": ratio("read_p99_us", "read_p99_us", invert=True),
        "write_p99_inflation": ratio("write_p99_us", "write_p99_us", invert=True),
        "read_p95_inflation": ratio("read_p95_us", "read_p95_us", invert=True),
        "write_p95_inflation": ratio("write_p95_us", "write_p95_us", invert=True)
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="I/O alone 과 I/O + NDP QoS 비교 실험을 실행한다."
    )
    parser.add_argument("--fio-job", type=Path, required=True, help="fio job 파일 경로")
    parser.add_argument("--fio-bin", default="fio", help="fio 실행 파일 경로")
    parser.add_argument("--input-dir", type=Path, default=Path("/mnt/nvme/openwebtext_for_ndt"))
    parser.add_argument("--input-glob", default="*.arrow")
    parser.add_argument("--ndp-output-dir", type=Path, default=Path("/mnt/nvme/openweb_bin_ndp"))
    parser.add_argument("--dev-path", default="/dev/ng0n1")
    parser.add_argument("--ndp-slots", default="1,2,4,8,16")
    parser.add_argument("--queue-depth", type=int, default=64)
    parser.add_argument("--max-inflight", type=int, default=0)
    parser.add_argument("--opcode", type=lambda x: int(x, 0), default=0xD4)
    parser.add_argument("--nsid", type=int, default=1)
    parser.add_argument("--admin", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--ndp-stop-timeout",
        type=float,
        default=0.0,
        help="0이면 현재 샤드 완료까지 무한 대기, 양수면 해당 초 후 강제 종료",
    )
    parser.add_argument("--no-cold-cache", action="store_true")
    parser.add_argument(
        "--result-root",
        type=Path,
        default=Path(__file__).resolve().parent / "qos_logs",
        help="실험결과_N 디렉터리를 생성할 상위 경로",
    )
    args = parser.parse_args()

    fio_path = shutil.which(args.fio_bin) if not os.path.isabs(args.fio_bin) else args.fio_bin
    if not fio_path:
        raise SystemExit(f"fio not found: {args.fio_bin}")
    if not args.fio_job.exists():
        raise SystemExit(f"fio job not found: {args.fio_job}")
    if not args.input_dir.exists():
        raise SystemExit(f"input dir not found: {args.input_dir}")

    input_paths = list_arrow_paths(args.input_dir, args.input_glob)
    if not input_paths:
        raise SystemExit(f"no arrow inputs found in {args.input_dir}")

    result_root = next_result_dir(args.result_root)
    result_root.mkdir(parents=True, exist_ok=False)

    slots_list = parse_slots(args.ndp_slots)

    meta = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "fio_job": str(args.fio_job),
        "fio_bin": str(fio_path),
        "input_dir": str(args.input_dir),
        "input_glob": args.input_glob,
        "ndp_output_dir": str(args.ndp_output_dir),
        "dev_path": args.dev_path,
        "ndp_slots": slots_list,
        "queue_depth": args.queue_depth,
        "max_inflight": args.max_inflight,
        "ndp_stop_timeout": args.ndp_stop_timeout,
    }
    (result_root / "meta.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")

    print(f"result_root={result_root}")
    print(f"ndp_slots={slots_list}")
    print("[RUN] scenario=io_alone")
    if not args.no_cold_cache:
        drop_linux_caches()
    io_alone_dir = result_root / "io_alone"
    io_alone_dir.mkdir(parents=True, exist_ok=True)
    io_alone = run_io_alone(str(fio_path), args.fio_job, io_alone_dir)

    io_plus_ndp_runs: List[Dict[str, object]] = []
    compare_rows: List[Dict[str, object]] = []
    for slots in slots_list:
        print(f"[RUN] scenario=io_plus_ndp slots={slots}")
        if not args.no_cold_cache:
            drop_linux_caches()
        io_plus_ndp_dir = result_root / f"io_plus_ndp_slots={slots}"
        io_plus_ndp_dir.mkdir(parents=True, exist_ok=True)
        io_plus_ndp = run_io_plus_ndp(
            fio_bin=str(fio_path),
            fio_job=args.fio_job,
            run_dir=io_plus_ndp_dir,
            input_paths=input_paths,
            ndp_output_dir=args.ndp_output_dir / f"slots_{slots}",
            dev_path=args.dev_path,
            ndp_slots=slots,
            queue_depth=args.queue_depth,
            max_inflight=args.max_inflight,
            opcode=args.opcode,
            nsid=args.nsid,
            admin=args.admin,
            verbose=args.verbose,
            stop_timeout_s=args.ndp_stop_timeout,
        )
        io_plus_ndp_runs.append(io_plus_ndp)
        compare_rows.append(
            {
                "slots": slots,
                "fio": io_plus_ndp["fio"],
                "ndp": io_plus_ndp["ndp"],
                "degradation": compute_degradation(io_alone, io_plus_ndp["fio"]),
            }
        )

    compare = {
        "io_alone": io_alone,
        "io_plus_ndp_runs": io_plus_ndp_runs,
        "compare_rows": compare_rows,
    }
    (result_root / "compare.json").write_text(
        json.dumps(compare, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    print(f"saved={result_root}")


if __name__ == "__main__":
    mp.set_start_method("spawn")
    main()
