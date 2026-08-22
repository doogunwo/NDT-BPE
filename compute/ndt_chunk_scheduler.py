#!/usr/bin/env python3
"""
단일 Arrow 파일을 record batch 단위로 분할하고, NDP slot 풀에 배정해 병렬 처리한 뒤
chunk별 .bin 결과를 순서대로 병합하는 Python 스케줄러.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Dict, List, Optional

import pyarrow as pa
import pyarrow.ipc as ipc

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import ndt_compute as nc


FIXED_SLOT: Optional[int] = None


def init_chunk_worker(slot_queue) -> None:
    global FIXED_SLOT
    FIXED_SLOT = int(slot_queue.get())


def mount_source(path: Path) -> str:
    target = path if path.exists() else path.parent
    result = subprocess.run(
        ["df", "--output=source", str(target)],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"failed to detect mount source for {target}")
    return lines[1]


def open_arrow_reader(input_path: Path):
    mmap = pa.memory_map(str(input_path), "r")
    try:
        # Hugging Face datasets shards are Arrow IPC streams.  Keep stream first;
        # fall back to file format for manually materialized Arrow files.
        try:
            reader = ipc.open_stream(mmap)
            return mmap, reader, "stream"
        except Exception:
            reader = ipc.open_file(mmap)
            return mmap, reader, "file"
    except Exception:
        mmap.close()
        raise


def list_arrow_record_batches(input_path: Path) -> List[Dict[str, object]]:
    mmap, reader, reader_kind = open_arrow_reader(input_path)
    chunks: List[Dict[str, object]] = []
    try:
        if reader_kind == "stream":
            iterator = enumerate(reader)
        else:
            iterator = ((i, reader.get_batch(i)) for i in range(reader.num_record_batches))

        for idx, batch in iterator:
            chunks.append(
                {
                    "chunk_index": idx,
                    "input_path": str(input_path),
                    "rows": batch.num_rows,
                    "input_bytes": input_path.stat().st_size,
                }
            )
    finally:
        try:
            reader.close()
        except Exception:
            pass
        mmap.close()

    return chunks


def process_chunk_file(
    chunk_index: int,
    input_path: str,
    output_path: str,
    dev_path: str,
    slots: int,
    queue_depth: int,
    opcode: int,
    nsid: int,
    admin: bool,
    verbose: bool,
    arrow_batch_start: int,
    arrow_batch_count: int,
) -> Dict[str, object]:
    global FIXED_SLOT
    if FIXED_SLOT is None:
        raise RuntimeError("worker fixed_slot is not initialized")

    start = time.perf_counter()
    result = nc.tokenize_to_nvme(
        dev_path=dev_path,
        input_path=input_path,
        output_path=output_path,
        queue_depth=queue_depth,
        max_inflight=1,
        slots=slots,
        fixed_slot=FIXED_SLOT,
        opcode=opcode,
        nsid=nsid,
        arrow_batch_start=arrow_batch_start,
        arrow_batch_count=arrow_batch_count,
        admin=admin,
        verbose=verbose,
    )
    elapsed_s = time.perf_counter() - start
    return {
        "chunk_index": chunk_index,
        "slot": FIXED_SLOT,
        "input_path": input_path,
        "output_path": output_path,
        "input_bytes": int(result["total_bytes"]),
        "segments": int(result["segments"]),
        "errors": int(result["errors"]),
        "elapsed_s_wall": elapsed_s,
        "elapsed_s_device": float(result["elapsed_us"]) / 1_000_000.0,
        "output_bytes": Path(output_path).stat().st_size if Path(output_path).exists() else 0,
        "output_valid_bytes": int(result.get("output_valid_bytes", 0)),
        "tokens": int(result.get("tokens", 0)),
    }


def merge_part_bins(part_rows: List[Dict[str, object]], output_path: Path, *, cleanup_parts: bool) -> int:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    total_written = 0
    with output_path.open("wb") as f_out:
        for row in sorted(part_rows, key=lambda item: int(item["chunk_index"])):
            part_path = Path(row["output_path"])
            with part_path.open("rb") as f_in:
                shutil.copyfileobj(f_in, f_out, length=1024 * 1024)
            total_written += part_path.stat().st_size
            if cleanup_parts:
                part_path.unlink(missing_ok=True)
        f_out.flush()
    return total_written


def _tokenize_arrow_file_chunked(
    *,
    input_path: str,
    output_path: str,
    dev_path: str,
    slots: int = 16,
    queue_depth: int = 64,
    opcode: int = 0xD4,
    nsid: int = 1,
    admin: bool = False,
    verbose: bool = False,
    temp_root: Optional[str] = None,
    keep_temp: bool = False,
    no_merge: bool = False,
    executor: Optional[ProcessPoolExecutor] = None,
) -> Dict[str, object]:
    if slots < 1:
        raise ValueError("slots must be >= 1")

    input_path_obj = Path(input_path)
    output_path_obj = Path(output_path)

    if temp_root:
        workspace = Path(temp_root)
        workspace.mkdir(parents=True, exist_ok=True)
    else:
        workspace = output_path_obj.parent / ".ndt_chunk_sched"
        workspace.mkdir(parents=True, exist_ok=True)

    input_src = mount_source(input_path_obj)
    output_src = mount_source(output_path_obj)
    workspace_src = mount_source(workspace)
    if not (input_src == output_src == workspace_src):
        raise RuntimeError(
            "input/output/temp_root must be on the same mounted device: "
            f"input={input_src} output={output_src} temp={workspace_src}"
        )

    part_dir = workspace / "parts"
    part_dir.mkdir(parents=True, exist_ok=True)

    chunks = list_arrow_record_batches(input_path_obj)
    if not chunks:
        raise RuntimeError(f"no record batches found in {input_path}")

    worker_count = min(slots, len(chunks))
    start_total = time.perf_counter()
    owns_executor = executor is None
    manager = None

    try:
        if executor is None:
            manager = mp.Manager()
            slot_queue = manager.Queue()
            for slot_id in range(worker_count):
                slot_queue.put(slot_id)
            executor = ProcessPoolExecutor(
                max_workers=worker_count,
                initializer=init_chunk_worker,
                initargs=(slot_queue,),
            )
        rows = list(
            executor.map(
                process_chunk_file,
                [int(chunk["chunk_index"]) for chunk in chunks],
                [str(chunk["input_path"]) for chunk in chunks],
                [str(part_dir / f"chunk_{int(chunk['chunk_index']):05d}.bin") for chunk in chunks],
                [dev_path] * len(chunks),
                [slots] * len(chunks),
                [queue_depth] * len(chunks),
                [opcode] * len(chunks),
                [nsid] * len(chunks),
                [admin] * len(chunks),
                [verbose] * len(chunks),
                [int(chunk["chunk_index"]) for chunk in chunks],
                [1] * len(chunks),
                chunksize=1,
            )
        )
    finally:
        if owns_executor and executor is not None:
            executor.shutdown(wait=True, cancel_futures=False)
        if manager is not None:
            manager.shutdown()

    elapsed_s = time.perf_counter() - start_total
    total_bytes = sum(int(row["input_bytes"]) for row in rows)
    total_segments = sum(int(row["segments"]) for row in rows)
    total_errors = sum(int(row["errors"]) for row in rows)
    total_elapsed_device = sum(float(row["elapsed_s_device"]) for row in rows)
    total_output_valid_bytes = sum(int(row.get("output_valid_bytes", 0)) for row in rows)
    total_tokens = sum(int(row.get("tokens", 0)) for row in rows)
    throughput_MBps = 0.0
    if elapsed_s > 0:
        throughput_MBps = (total_bytes / (1024.0 * 1024.0)) / elapsed_s

    failed_rows = [row for row in rows if int(row["errors"]) > 0]
    output_bytes = 0
    try:
        if failed_rows:
            summary = {
                "backend": "ndp_chunk_scheduler",
                "input_path": str(input_path_obj),
                "output_path": str(output_path_obj),
                "dev_path": dev_path,
                "slots": slots,
                "worker_count": worker_count,
                "queue_depth": queue_depth,
                "num_chunks": len(chunks),
                "total_bytes": total_bytes,
                "total_segments": total_segments,
                "total_errors": total_errors,
                "elapsed_s": elapsed_s,
                "device_elapsed_s_sum": total_elapsed_device,
                "throughput_MBps": throughput_MBps,
                "output_bytes": output_bytes,
                "output_valid_bytes": total_output_valid_bytes,
                "tokens": total_tokens,
                "rows": rows,
                "workspace": str(workspace),
                "failed_chunks": [int(row["chunk_index"]) for row in failed_rows],
                "workspace_retained": True,
            }
            raise RuntimeError(json.dumps(summary, ensure_ascii=False))

        if no_merge:
            output_bytes = 0
        else:
            output_bytes = merge_part_bins(rows, output_path_obj, cleanup_parts=not keep_temp)

        summary = {
            "backend": "ndp_chunk_scheduler",
            "input_path": str(input_path_obj),
            "output_path": str(output_path_obj),
            "dev_path": dev_path,
            "slots": slots,
            "worker_count": worker_count,
            "queue_depth": queue_depth,
            "num_chunks": len(chunks),
            "total_bytes": total_bytes,
            "total_segments": total_segments,
            "total_errors": total_errors,
            "elapsed_s": elapsed_s,
            "device_elapsed_s_sum": total_elapsed_device,
            "throughput_MBps": throughput_MBps,
            "output_bytes": output_bytes,
            "output_valid_bytes": total_output_valid_bytes,
            "tokens": total_tokens,
            "merged": not no_merge,
            "rows": rows,
            "workspace": str(workspace),
        }
        return summary
    finally:
        if not keep_temp and not failed_rows:
            shutil.rmtree(workspace, ignore_errors=True)


class NdtChunkScheduler:
    """Reusable per-actor chunk scheduler with a persistent slot process pool."""

    def __init__(self, slots: int = 16):
        if slots < 1:
            raise ValueError("slots must be >= 1")
        self.slots = slots
        self._manager = mp.Manager()
        self._slot_queue = self._manager.Queue()
        for slot_id in range(slots):
            self._slot_queue.put(slot_id)
        self._executor = ProcessPoolExecutor(
            max_workers=slots,
            initializer=init_chunk_worker,
            initargs=(self._slot_queue,),
        )

    def close(self) -> None:
        self._executor.shutdown(wait=True, cancel_futures=True)
        self._manager.shutdown()

    def tokenize_arrow_file_chunked(self, **kwargs) -> Dict[str, object]:
        kwargs.setdefault("slots", self.slots)
        if int(kwargs["slots"]) != self.slots:
            raise ValueError("persistent scheduler slots cannot change per call")
        return _tokenize_arrow_file_chunked(executor=self._executor, **kwargs)

    def __enter__(self) -> "NdtChunkScheduler":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


def tokenize_arrow_file_chunked(**kwargs) -> Dict[str, object]:
    slots = int(kwargs.get("slots", 16))
    with NdtChunkScheduler(slots=slots) as scheduler:
        return scheduler.tokenize_arrow_file_chunked(**kwargs)


def main() -> None:
    parser = argparse.ArgumentParser(description="Single-file Arrow NDP chunk scheduler")
    parser.add_argument("--input-path", required=True)
    parser.add_argument("--output-path", required=True)
    parser.add_argument("--dev-path", default="/dev/ng2n1")
    parser.add_argument("--slots", type=int, default=16)
    parser.add_argument("--queue-depth", type=int, default=64)
    parser.add_argument("--opcode", type=lambda x: int(x, 0), default=0xD4)
    parser.add_argument("--nsid", type=int, default=1)
    parser.add_argument("--admin", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--temp-root", default=None)
    parser.add_argument("--keep-temp", action="store_true")
    parser.add_argument("--no-merge", action="store_true", help="skip final part-file merge and print summary only")
    args = parser.parse_args()

    summary = tokenize_arrow_file_chunked(
        input_path=args.input_path,
        output_path=args.output_path,
        dev_path=args.dev_path,
        slots=args.slots,
        queue_depth=args.queue_depth,
        opcode=args.opcode,
        nsid=args.nsid,
        admin=args.admin,
        verbose=args.verbose,
        temp_root=args.temp_root,
        keep_temp=args.keep_temp,
        no_merge=args.no_merge,
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
