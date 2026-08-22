#!/usr/bin/env python3
"""
ndt_compute.tokenize_to_nvme()를 슬롯 수별로 반복 실행하는 NDP sweep 스크립트.
"""

import argparse
import csv
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional


REPO_ROOT = Path(__file__).resolve().parent.parent
COMPUTE_DIR = REPO_ROOT / "compute"
if str(COMPUTE_DIR) not in sys.path:
    sys.path.insert(0, str(COMPUTE_DIR))

import ndt_compute as nc


DEFAULT_SLOTS = "1,2,4,8,16,32,64"
RESULT_DIR_PREFIX = "실험결과_"
MPSTAT_INTERVAL_SEC = 1
IOSTAT_INTERVAL_SEC = 1


def parse_slots(value: str) -> List[int]:
    slots: List[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        slot = int(item)
        if slot < 1:
            raise ValueError("slot 수는 1 이상이어야 합니다.")
        slots.append(slot)
    if not slots:
        raise ValueError("slot 목록이 비어 있습니다.")
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


def start_mpstat(log_path: Path) -> subprocess.Popen:
    with log_path.open("w", encoding="utf-8") as f:
        f.write(f"# mpstat start {datetime.now().isoformat(timespec='seconds')}\n")
        f.flush()

    log_fp = log_path.open("a", encoding="utf-8")
    return subprocess.Popen(
        ["mpstat", "-P", "ALL", str(MPSTAT_INTERVAL_SEC)],
        stdout=log_fp,
        stderr=subprocess.STDOUT,
        text=True,
        preexec_fn=os.setsid,
    )


def stop_mpstat(proc: subprocess.Popen) -> None:
    try:
        os.killpg(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=5)


def start_iostat(log_path: Path, device: Optional[str]) -> Optional[subprocess.Popen]:
    if shutil.which("iostat") is None:
        return None

    with log_path.open("w", encoding="utf-8") as f:
        f.write(f"# iostat start {datetime.now().isoformat(timespec='seconds')}\n")
        f.flush()

    cmd = ["iostat", "-dx", "-y", str(IOSTAT_INTERVAL_SEC)]
    if device:
        cmd.append(device)

    log_fp = log_path.open("a", encoding="utf-8")
    return subprocess.Popen(
        cmd,
        stdout=log_fp,
        stderr=subprocess.STDOUT,
        text=True,
        preexec_fn=os.setsid,
    )


def stop_iostat(proc: Optional[subprocess.Popen]) -> None:
    if proc is None:
        return
    try:
        os.killpg(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=5)


def parse_mpstat_summary(log_path: Path, slots: int) -> Dict[str, object]:
    cpu_count = os.cpu_count() or 1
    average_lines = []
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("Average:"):
            average_lines.append(line)

    def to_float(value: str) -> Optional[float]:
        try:
            return float(value)
        except ValueError:
            return None

    per_cpu: Dict[str, Dict[str, float]] = {}
    for line in average_lines:
        parts = line.split()
        if len(parts) < 12:
            continue
        cpu_id = parts[1]
        if cpu_id == "CPU" or parts[-1] == "%idle":
            continue
        idle = to_float(parts[-1])
        usr = to_float(parts[2])
        sys_pct = to_float(parts[4])
        iowait = to_float(parts[5])
        if idle is None or usr is None or sys_pct is None or iowait is None:
            continue
        busy = 100.0 - idle
        per_cpu[cpu_id] = {
            "usr_pct": usr,
            "sys_pct": sys_pct,
            "iowait_pct": iowait,
            "idle_pct": idle,
            "busy_pct": busy,
        }

    all_cpu = per_cpu.get("all", {})
    busy_pct_all = float(all_cpu.get("busy_pct", 0.0))
    busy_core_equiv = cpu_count * busy_pct_all / 100.0
    target_parallelism = min(slots, cpu_count)
    efficiency_pct = 0.0
    if target_parallelism > 0:
        efficiency_pct = min(100.0, busy_core_equiv / target_parallelism * 100.0)

    return {
        "logical_cpus": cpu_count,
        "avg_cpu_busy_pct_all": busy_pct_all,
        "avg_busy_core_equivalent": busy_core_equiv,
        "efficiency_pct_vs_slots": efficiency_pct,
        "per_cpu": per_cpu,
    }


def parse_iostat_summary(log_path: Path, device: Optional[str]) -> Dict[str, object]:
    if not log_path.exists():
        return {
            "device": device,
            "sample_count": 0,
            "avg_read_MBps": 0.0,
            "avg_write_MBps": 0.0,
            "avg_total_MBps": 0.0,
            "avg_r_await_ms": 0.0,
            "avg_w_await_ms": 0.0,
            "avg_aqu_sz": 0.0,
            "avg_util_pct": 0.0,
        }

    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    header_cols: List[str] = []
    rows: List[Dict[str, float]] = []

    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#") or line.startswith("Linux") or line.startswith("avg-cpu:"):
            continue
        if line.startswith("Device"):
            header_cols = line.split()
            continue
        if not header_cols:
            continue

        tokens = line.split()
        if len(tokens) < 2:
            continue
        dev_name = tokens[0]
        if device and dev_name != device:
            continue

        metric_map: Dict[str, float] = {}
        usable = min(len(header_cols) - 1, len(tokens) - 1)
        for i in range(usable):
            key = header_cols[i + 1]
            val = tokens[i + 1]
            try:
                metric_map[key] = float(val)
            except ValueError:
                metric_map[key] = 0.0
        if metric_map:
            rows.append(metric_map)

    if not rows:
        return {
            "device": device,
            "sample_count": 0,
            "avg_read_MBps": 0.0,
            "avg_write_MBps": 0.0,
            "avg_total_MBps": 0.0,
            "avg_r_await_ms": 0.0,
            "avg_w_await_ms": 0.0,
            "avg_aqu_sz": 0.0,
            "avg_util_pct": 0.0,
        }

    def avg_of(key: str) -> float:
        values = [row.get(key, 0.0) for row in rows]
        return sum(values) / len(values)

    avg_r_kBps = avg_of("rkB/s")
    avg_w_kBps = avg_of("wkB/s")

    return {
        "device": device,
        "sample_count": len(rows),
        "avg_read_MBps": avg_r_kBps / 1024.0,
        "avg_write_MBps": avg_w_kBps / 1024.0,
        "avg_total_MBps": (avg_r_kBps + avg_w_kBps) / 1024.0,
        "avg_r_await_ms": avg_of("r_await"),
        "avg_w_await_ms": avg_of("w_await"),
        "avg_aqu_sz": avg_of("aqu-sz"),
        "avg_util_pct": avg_of("%util"),
    }


def detect_iostat_device(input_dir: Path) -> Optional[str]:
    result = subprocess.run(
        ["df", "--output=source", str(input_dir)],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        return None
    source = lines[1]
    if not source.startswith("/dev/"):
        return None
    return Path(source).name


def ndp_stats_file_path(stats_dir: Path, kind: str, slots: int) -> Path:
    suffix = "csv" if kind == "per_file" else "json"
    return stats_dir / f"run_01_{kind}_(슬롯개수={slots}).{suffix}"


def run_ndp_once(
    arrow_paths: List[Path],
    output_dir: Path,
    dev_path: str,
    slots: int,
    queue_depth: int,
    max_inflight: int,
    opcode: int,
    nsid: int,
    admin: bool,
    verbose: bool,
    stats_dir: Path,
    run_log_path: Path,
    progress: bool,
) -> Dict[str, object]:
    stats_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows: List[Dict[str, object]] = []
    total_bytes = 0
    total_segments = 0
    total_errors = 0
    device_elapsed_us = 0.0

    start = time.perf_counter()
    with run_log_path.open("w", encoding="utf-8") as run_log:
        run_log.write(f"=== RUN START {datetime.now().isoformat(timespec='seconds')} ===\n")
        run_log.write(
            f"dev_path={dev_path} slots={slots} queue_depth={queue_depth} "
            f"max_inflight={max_inflight if max_inflight > 0 else slots} input_files={len(arrow_paths)}\n"
        )
        run_log.flush()

        for idx, arrow_path in enumerate(arrow_paths, start=1):
            out_bin = output_dir / f"{arrow_path.stem}.bin"
            shard_start = time.perf_counter()
            result = nc.tokenize_to_nvme(
                dev_path=dev_path,
                input_path=str(arrow_path),
                output_path=str(out_bin),
                opcode=opcode,
                nsid=nsid,
                queue_depth=queue_depth,
                max_inflight=max_inflight,
                slots=slots,
                admin=admin,
                verbose=verbose,
            )
            shard_elapsed_s = time.perf_counter() - shard_start

            bytes_this = int(result["total_bytes"])
            errors_this = int(result["errors"])
            segments_this = int(result["segments"])
            elapsed_us_this = float(result["elapsed_us"])

            total_bytes += bytes_this
            total_errors += errors_this
            total_segments += segments_this
            device_elapsed_us += elapsed_us_this

            rows.append(
                {
                    "index": idx,
                    "input_path": str(arrow_path),
                    "output_path": str(out_bin),
                    "input_bytes": bytes_this,
                    "segments": segments_this,
                    "errors": errors_this,
                    "elapsed_s_wall": shard_elapsed_s,
                    "elapsed_s_device": elapsed_us_this / 1_000_000.0,
                }
            )

            if progress:
                print(f"[slots={slots}] {idx}/{len(arrow_paths)} {arrow_path.name}")

        elapsed_s = time.perf_counter() - start
        run_log.write(
            f"=== RUN END {datetime.now().isoformat(timespec='seconds')} "
            f"elapsed_wall_s={elapsed_s:.3f} total_bytes={total_bytes} "
            f"segments={total_segments} errors={total_errors} ===\n"
        )

    per_file_csv_path = ndp_stats_file_path(stats_dir, "per_file", slots)
    with per_file_csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "index",
                "input_path",
                "output_path",
                "input_bytes",
                "segments",
                "errors",
                "elapsed_s_wall",
                "elapsed_s_device",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    throughput_MBps = 0.0
    if elapsed_s > 0:
        throughput_MBps = (total_bytes / (1024.0 * 1024.0)) / elapsed_s

    summary = {
        "backend": "ndp",
        "slots": slots,
        "num_files": len(arrow_paths),
        "total_bytes": total_bytes,
        "total_segments": total_segments,
        "total_errors": total_errors,
        "elapsed_s": elapsed_s,
        "device_elapsed_s_sum": device_elapsed_us / 1_000_000.0,
        "throughput_MBps": throughput_MBps,
        "dev_path": dev_path,
        "queue_depth": queue_depth,
        "max_inflight": max_inflight if max_inflight > 0 else slots,
        "opcode": opcode,
        "nsid": nsid,
        "output_dir": str(output_dir),
        "per_file_csv": str(per_file_csv_path),
    }

    summary_json_path = ndp_stats_file_path(stats_dir, "summary", slots)
    summary_json_path.write_text(
        json.dumps(summary, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return summary


def run_one(
    arrow_paths: List[Path],
    output_root: Path,
    dev_path: str,
    slots: int,
    queue_depth: int,
    max_inflight: int,
    opcode: int,
    nsid: int,
    admin: bool,
    verbose: bool,
    result_root: Path,
    cold_cache: bool,
    iostat_device: Optional[str],
    progress: bool,
) -> Dict[str, object]:
    run_dir = result_root / f"slots={slots}"
    run_dir.mkdir(parents=True, exist_ok=True)

    ndp_stats_dir = run_dir / "ndp_logs"
    run_log_path = run_dir / "run.log"
    mpstat_log_path = run_dir / "mpstat.log"
    iostat_log_path = run_dir / "iostat.log"
    meta_path = run_dir / "meta.json"
    merged_summary_path = run_dir / "summary.json"
    slot_output_dir = output_root / f"slots_{slots}"

    if cold_cache:
        drop_linux_caches()

    meta = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "backend": "ndp",
        "slots": slots,
        "dev_path": dev_path,
        "queue_depth": queue_depth,
        "max_inflight": max_inflight if max_inflight > 0 else slots,
        "opcode": opcode,
        "nsid": nsid,
        "output_dir": str(slot_output_dir),
        "input_files": [str(path) for path in arrow_paths],
    }
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")

    mpstat_proc = start_mpstat(mpstat_log_path)
    iostat_proc = start_iostat(iostat_log_path, iostat_device)
    try:
        ndp_summary = run_ndp_once(
            arrow_paths=arrow_paths,
            output_dir=slot_output_dir,
            dev_path=dev_path,
            slots=slots,
            queue_depth=queue_depth,
            max_inflight=max_inflight,
            opcode=opcode,
            nsid=nsid,
            admin=admin,
            verbose=verbose,
            stats_dir=ndp_stats_dir,
            run_log_path=run_log_path,
            progress=progress,
        )
    finally:
        stop_mpstat(mpstat_proc)
        stop_iostat(iostat_proc)

    cpu_summary = parse_mpstat_summary(mpstat_log_path, slots)
    io_summary = parse_iostat_summary(iostat_log_path, iostat_device)
    merged_summary = {
        "slots": slots,
        "ndp": ndp_summary,
        "cpu": cpu_summary,
        "io": io_summary,
        "artifacts": {
            "run_log": str(run_log_path),
            "mpstat_log": str(mpstat_log_path),
            "iostat_log": str(iostat_log_path),
            "ndp_summary": str(ndp_stats_file_path(ndp_stats_dir, "summary", slots)),
        },
    }
    merged_summary_path.write_text(
        json.dumps(merged_summary, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return merged_summary


def write_master_summary(result_root: Path, rows: List[Dict[str, object]]) -> None:
    csv_path = result_root / "summary.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "slots",
                "elapsed_s",
                "device_elapsed_s_sum",
                "throughput_MBps",
                "total_bytes",
                "total_segments",
                "total_errors",
                "avg_cpu_busy_pct_all",
                "avg_busy_core_equivalent",
                "efficiency_pct_vs_slots",
                "io_total_MBps",
                "io_read_MBps",
                "io_write_MBps",
                "io_util_pct",
                "io_r_await_ms",
                "io_w_await_ms",
                "io_aqu_sz",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="ndt_compute.tokenize_to_nvme()를 슬롯 수별로 자동 실행하고 CPU/I/O 통계를 저장한다."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=Path("/mnt/nvme/openwebtext_for_ndt"),
        help="Arrow 샤드가 있는 입력 디렉터리.",
    )
    parser.add_argument(
        "--input-glob",
        default="*.arrow",
        help="입력 디렉터리 아래에서 찾을 Arrow 샤드 패턴.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/mnt/nvme/openweb_bin_ndp"),
        help="NDP 결과 .bin 출력 루트 디렉터리.",
    )
    parser.add_argument(
        "--dev-path",
        default="/dev/ng0n1",
        help="NDP NVMe-oF 디바이스 경로.",
    )
    parser.add_argument(
        "--slots",
        default=DEFAULT_SLOTS,
        help="쉼표로 구분한 슬롯 수 목록.",
    )
    parser.add_argument(
        "--queue-depth",
        type=int,
        default=64,
        help="io_uring queue depth.",
    )
    parser.add_argument(
        "--max-inflight",
        type=int,
        default=0,
        help="inflight 요청 수. 0이면 slots와 동일하게 맞춘다.",
    )
    parser.add_argument(
        "--opcode",
        type=lambda x: int(x, 0),
        default=0xD4,
        help="NDP 명령 opcode.",
    )
    parser.add_argument(
        "--nsid",
        type=int,
        default=1,
        help="NVMe namespace id.",
    )
    parser.add_argument(
        "--admin",
        action="store_true",
        help="admin queue opcode로 보낸다.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="ndt_compute verbose 출력을 켠다.",
    )
    parser.add_argument(
        "--no-progress",
        action="store_true",
        help="샤드 진행 출력을 끈다.",
    )
    parser.add_argument(
        "--no-cold-cache",
        action="store_true",
        help="각 슬롯 실험 전 cold-cache를 끈다.",
    )
    parser.add_argument(
        "--result-root",
        type=Path,
        default=Path(__file__).resolve().parent / "ndp_logs",
        help="실험결과_N 디렉터리를 생성할 상위 경로.",
    )
    parser.add_argument(
        "--iostat-device",
        default="auto",
        help="iostat 대상 디바이스명. auto면 입력 경로 기준 자동 탐지, none이면 비활성화.",
    )
    args = parser.parse_args()

    if not args.input_dir.exists():
        raise SystemExit(f"input dir not found: {args.input_dir}")

    arrow_paths = list_arrow_paths(args.input_dir, args.input_glob)
    if not arrow_paths:
        raise SystemExit(f"no input shards found: dir={args.input_dir} glob={args.input_glob}")

    slots_list = parse_slots(args.slots)
    if args.iostat_device.lower() == "auto":
        iostat_device = detect_iostat_device(args.input_dir)
    elif args.iostat_device.lower() in {"none", "off", "disable"}:
        iostat_device = None
    else:
        iostat_device = args.iostat_device

    result_root = next_result_dir(args.result_root)
    result_root.mkdir(parents=True, exist_ok=False)

    print(f"result_root={result_root}")
    print(f"slots={slots_list}")
    print(f"input_files={len(arrow_paths)}")
    print(f"iostat_device={iostat_device if iostat_device else 'disabled'}")

    master_rows: List[Dict[str, object]] = []
    for slots in slots_list:
        print(f"[RUN] slots={slots}")
        merged_summary = run_one(
            arrow_paths=arrow_paths,
            output_root=args.output_dir,
            dev_path=args.dev_path,
            slots=slots,
            queue_depth=args.queue_depth,
            max_inflight=args.max_inflight,
            opcode=args.opcode,
            nsid=args.nsid,
            admin=args.admin,
            verbose=args.verbose,
            result_root=result_root,
            cold_cache=not args.no_cold_cache,
            iostat_device=iostat_device,
            progress=not args.no_progress,
        )
        master_rows.append(
            {
                "slots": slots,
                "elapsed_s": merged_summary["ndp"]["elapsed_s"],
                "device_elapsed_s_sum": merged_summary["ndp"]["device_elapsed_s_sum"],
                "throughput_MBps": merged_summary["ndp"]["throughput_MBps"],
                "total_bytes": merged_summary["ndp"]["total_bytes"],
                "total_segments": merged_summary["ndp"]["total_segments"],
                "total_errors": merged_summary["ndp"]["total_errors"],
                "avg_cpu_busy_pct_all": merged_summary["cpu"]["avg_cpu_busy_pct_all"],
                "avg_busy_core_equivalent": merged_summary["cpu"]["avg_busy_core_equivalent"],
                "efficiency_pct_vs_slots": merged_summary["cpu"]["efficiency_pct_vs_slots"],
                "io_total_MBps": merged_summary["io"]["avg_total_MBps"],
                "io_read_MBps": merged_summary["io"]["avg_read_MBps"],
                "io_write_MBps": merged_summary["io"]["avg_write_MBps"],
                "io_util_pct": merged_summary["io"]["avg_util_pct"],
                "io_r_await_ms": merged_summary["io"]["avg_r_await_ms"],
                "io_w_await_ms": merged_summary["io"]["avg_w_await_ms"],
                "io_aqu_sz": merged_summary["io"]["avg_aqu_sz"],
            }
        )
        write_master_summary(result_root, master_rows)

    print(f"saved={result_root}")


if __name__ == "__main__":
    main()
