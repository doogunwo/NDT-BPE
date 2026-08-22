#!/usr/bin/env python3
"""
ndt-bpe_baseline.py를 여러 worker 수로 반복 실행하는 baseline sweep 스크립트.
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


DEFAULT_WORKERS = "1,2,4,16,32,64"
BASELINE_SCRIPT = Path(__file__).resolve().parent / "ndt-bpe_baseline.py"
RESULT_DIR_PREFIX = "실험결과_"
WAIT_POLL_SEC = 10
MPSTAT_INTERVAL_SEC = 1
IOSTAT_INTERVAL_SEC = 1


def parse_workers(value: str) -> List[int]:
    workers: List[int] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        worker = int(item)
        if worker < 1:
            raise ValueError("worker 수는 1 이상이어야 합니다.")
        workers.append(worker)
    if not workers:
        raise ValueError("worker 목록이 비어 있습니다.")
    return workers


def next_result_dir(root: Path) -> Path:
    idx = 1
    while True:
        candidate = root / f"{RESULT_DIR_PREFIX}{idx}"
        if not candidate.exists():
            return candidate
        idx += 1


def running_baseline_processes(input_dir: Path) -> List[str]:
    result = subprocess.run(
        ["ps", "-eo", "pid=,cmd="],
        check=True,
        capture_output=True,
        text=True,
    )
    matches: List[str] = []
    for line in result.stdout.splitlines():
        if "ndt-bpe_baseline.py" not in line:
            continue
        if str(input_dir) not in line:
            continue
        matches.append(line.strip())
    return matches


def wait_for_active_baseline_to_finish(input_dir: Path) -> None:
    while True:
        matches = running_baseline_processes(input_dir)
        if not matches:
            return
        print("현재 실행 중인 baseline이 끝나기를 기다리는 중입니다.")
        for line in matches:
            print(f"  active={line}")
        time.sleep(WAIT_POLL_SEC)


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


def parse_mpstat_summary(log_path: Path, workers: int) -> Dict[str, object]:
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
    target_parallelism = min(workers, cpu_count)
    efficiency_pct = 0.0
    if target_parallelism > 0:
        efficiency_pct = min(100.0, busy_core_equiv / target_parallelism * 100.0)

    return {
        "logical_cpus": cpu_count,
        "avg_cpu_busy_pct_all": busy_pct_all,
        "avg_busy_core_equivalent": busy_core_equiv,
        "efficiency_pct_vs_workers": efficiency_pct,
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


def build_baseline_cmd(
    input_dir: Path,
    output_dir: Path,
    workers: int,
    stats_dir: Path,
    arrow_text_column: str,
    no_progress: bool,
    cold_cache: bool,
) -> List[str]:
    cmd = [
        sys.executable,
        str(BASELINE_SCRIPT),
        "--backend",
        "cpu",
        "--input-dir",
        str(input_dir),
        "--output-dir",
        str(output_dir),
        "--threads",
        str(workers),
        "--repeat",
        "1",
        "--stats-dir",
        str(stats_dir),
        "--result-suffix",
        f"_(프로세스개수={workers})",
        "--arrow-text-column",
        arrow_text_column,
    ]
    if no_progress:
        cmd.append("--no-progress")
    if not cold_cache:
        cmd.append("--no-cold-cache")
    return cmd


def run_one(
    input_dir: Path,
    output_dir: Path,
    workers: int,
    result_root: Path,
    arrow_text_column: str,
    no_progress: bool,
    cold_cache: bool,
    iostat_device: Optional[str],
) -> Dict[str, object]:
    run_dir = result_root / f"workers={workers}"
    run_dir.mkdir(parents=True, exist_ok=True)

    baseline_stats_dir = run_dir / "baseline_logs"
    run_log_path = run_dir / "run.log"
    mpstat_log_path = run_dir / "mpstat.log"
    iostat_log_path = run_dir / "iostat.log"
    meta_path = run_dir / "meta.json"
    merged_summary_path = run_dir / "summary.json"

    cmd = build_baseline_cmd(
        input_dir=input_dir,
        output_dir=output_dir,
        workers=workers,
        stats_dir=baseline_stats_dir,
        arrow_text_column=arrow_text_column,
        no_progress=no_progress,
        cold_cache=cold_cache,
    )

    meta = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "workers": workers,
        "input_dir": str(input_dir),
        "output_dir": str(output_dir),
        "cmd": cmd,
    }
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")

    start = time.time()
    mpstat_proc = start_mpstat(mpstat_log_path)
    iostat_proc = start_iostat(iostat_log_path, iostat_device)
    try:
        with run_log_path.open("w", encoding="utf-8") as run_log:
            run_log.write(f"=== RUN START {datetime.now().isoformat(timespec='seconds')} ===\n")
            run_log.write("$ " + " ".join(cmd) + "\n\n")
            run_log.flush()
            proc = subprocess.Popen(
                cmd,
                stdout=run_log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            rc = proc.wait()
            elapsed_wall = time.time() - start
            run_log.write("\n")
            run_log.write(
                f"=== RUN END {datetime.now().isoformat(timespec='seconds')} rc={rc} "
                f"elapsed_wall_s={elapsed_wall:.3f} ===\n"
            )
    finally:
        stop_mpstat(mpstat_proc)
        stop_iostat(iostat_proc)

    if rc != 0:
        raise RuntimeError(f"baseline run failed for workers={workers} (rc={rc})")

    baseline_summary_path = baseline_stats_dir / f"run_01_summary_(프로세스개수={workers}).json"
    baseline_summary = json.loads(baseline_summary_path.read_text(encoding="utf-8"))
    cpu_summary = parse_mpstat_summary(mpstat_log_path, workers)
    io_summary = parse_iostat_summary(iostat_log_path, iostat_device)

    merged_summary = {
        "workers": workers,
        "baseline": baseline_summary,
        "cpu": cpu_summary,
        "io": io_summary,
        "artifacts": {
            "run_log": str(run_log_path),
            "mpstat_log": str(mpstat_log_path),
            "iostat_log": str(iostat_log_path),
            "baseline_summary": str(baseline_summary_path),
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
                "workers",
                "elapsed_s",
                "throughput_MBps",
                "throughput_tokens_per_s",
                "avg_cpu_busy_pct_all",
                "avg_busy_core_equivalent",
                "efficiency_pct_vs_workers",
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
        description="ndt-bpe_baseline.py를 worker 수별로 자동 실행하고 CPU 통계를 저장한다."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=Path("/mnt/nvme/openwebtext_for_ndt"),
        help="입력 디렉터리 경로.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/mnt/nvme/openweb_bin"),
        help="baseline 결과 .bin 출력 디렉터리.",
    )
    parser.add_argument(
        "--workers",
        default=DEFAULT_WORKERS,
        help="쉼표로 구분한 worker 수 목록.",
    )
    parser.add_argument(
        "--arrow-text-column",
        default="text",
        help="Arrow 입력에서 사용할 text 컬럼 이름.",
    )
    parser.add_argument(
        "--no-progress",
        action="store_true",
        help="하위 baseline 실행에서 progress 출력을 끈다.",
    )
    parser.add_argument(
        "--no-cold-cache",
        action="store_true",
        help="하위 baseline 실행에서 cold-cache를 끈다.",
    )
    parser.add_argument(
        "--result-root",
        type=Path,
        default=Path(__file__).resolve().parent / "baseline_logs",
        help="실험결과_N 디렉터리를 생성할 상위 경로.",
    )
    parser.add_argument(
        "--iostat-device",
        default="auto",
        help="iostat 대상 디바이스명. auto면 입력 경로 기준 자동 탐지, none이면 비활성화.",
    )
    args = parser.parse_args()

    if not BASELINE_SCRIPT.exists():
        raise SystemExit(f"baseline script not found: {BASELINE_SCRIPT}")
    if not args.input_dir.exists():
        raise SystemExit(f"input dir not found: {args.input_dir}")

    workers = parse_workers(args.workers)
    if args.iostat_device.lower() == "auto":
        iostat_device = detect_iostat_device(args.input_dir)
    elif args.iostat_device.lower() in {"none", "off", "disable"}:
        iostat_device = None
    else:
        iostat_device = args.iostat_device

    result_root = next_result_dir(args.result_root)
    result_root.mkdir(parents=True, exist_ok=False)

    print(f"result_root={result_root}")
    print(f"workers={workers}")
    print(f"iostat_device={iostat_device if iostat_device else 'disabled'}")
    wait_for_active_baseline_to_finish(args.input_dir)

    master_rows: List[Dict[str, object]] = []
    for worker in workers:
        print(f"[RUN] workers={worker}")
        merged_summary = run_one(
            input_dir=args.input_dir,
            output_dir=args.output_dir,
            workers=worker,
            result_root=result_root,
            arrow_text_column=args.arrow_text_column,
            no_progress=args.no_progress,
            cold_cache=not args.no_cold_cache,
            iostat_device=iostat_device,
        )
        master_rows.append(
            {
                "workers": worker,
                "elapsed_s": merged_summary["baseline"]["elapsed_s"],
                "throughput_MBps": merged_summary["baseline"]["throughput_MBps"],
                "throughput_tokens_per_s": merged_summary["baseline"]["throughput_tokens_per_s"],
                "avg_cpu_busy_pct_all": merged_summary["cpu"]["avg_cpu_busy_pct_all"],
                "avg_busy_core_equivalent": merged_summary["cpu"]["avg_busy_core_equivalent"],
                "efficiency_pct_vs_workers": merged_summary["cpu"]["efficiency_pct_vs_workers"],
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
