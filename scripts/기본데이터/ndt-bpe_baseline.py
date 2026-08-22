#!/usr/bin/env python3
"""
Arrow/text 입력을 CPU에서 토크나이즈하고 .bin으로 저장하는 baseline 스크립트.
"""

import argparse
import csv
import json
import os
import struct
import subprocess
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Dict, Iterable, List, Optional

import pyarrow as pa
import pyarrow.ipc as ipc
from tokenizers import Tokenizer


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODEL = SCRIPT_DIR.parent / "storage" / "runtime" / "model" / "bbpe_tokenizer.json"
DEFAULT_INPUT_DIR = Path("/mnt/nvme/openwebtext_for_ndt")
DEFAULT_OUTPUT_DIR = Path("/mnt/nvme/openweb_bin")
DEFAULT_STATS_DIR = SCRIPT_DIR / "baseline_logs"

TOKENIZER: Optional[Tokenizer] = None


def texts_from_arrow_array(arr: pa.Array) -> List[str]:
    if pa.types.is_string(arr.type) or pa.types.is_large_string(arr.type):
        try:
            return [text for text in arr.to_pylist() if text]
        except UnicodeDecodeError:
            offsets_buf = arr.buffers()[1]
            data_buf = arr.buffers()[2]
            if offsets_buf is None or data_buf is None:
                return []
            offset_width = 8 if pa.types.is_large_string(arr.type) else 4
            fmt_char = "q" if offset_width == 8 else "i"
            values_count = len(arr) + 1
            start = arr.offset * offset_width
            stop = start + values_count * offset_width
            raw_offsets = offsets_buf.to_pybytes()[start:stop]
            offsets = list(struct.unpack(f"<{values_count}{fmt_char}", raw_offsets))
            data = data_buf.to_pybytes()
            texts: List[str] = []
            for i in range(len(offsets) - 1):
                if not arr[i].is_valid:
                    continue
                start = offsets[i]
                end = offsets[i + 1]
                text = data[start:end].decode("utf-8", errors="ignore")
                if text:
                    texts.append(text)
            return texts
    return [text for text in arr.to_pylist() if text]


def drop_linux_caches() -> None:
    if os.geteuid() != 0:
        raise SystemExit("cold-cache 실행은 root 권한이 필요합니다.")
    subprocess.run(["sync"], check=True)
    Path("/proc/sys/vm/drop_caches").write_text("3\n", encoding="utf-8")


def init_worker(model_path: str) -> None:
    global TOKENIZER
    TOKENIZER = Tokenizer.from_file(model_path)


def iter_input_files(input_dir: Path) -> List[Path]:
    files = []
    for path in input_dir.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() not in {".arrow", ".txt", ".text"}:
            continue
        files.append(path)
    return sorted(files)


def output_path_for(input_dir: Path, output_dir: Path, input_path: Path) -> Path:
    rel = input_path.relative_to(input_dir)
    out_path = output_dir / rel
    return out_path.with_suffix(".bin")


def pack_ids_to_bin(ids: Iterable[int], out_file) -> int:
    count = 0
    buf = bytearray()
    for token_id in ids:
        buf.extend(struct.pack("<I", int(token_id)))
        count += 1
    if buf:
        out_file.write(buf)
    return count


def encode_texts(texts: List[str], out_file) -> int:
    global TOKENIZER
    if TOKENIZER is None:
        raise RuntimeError("tokenizer is not initialized")
    total_tokens = 0
    if not texts:
        return 0
    encodings = TOKENIZER.encode_batch(texts)
    for enc in encodings:
        total_tokens += pack_ids_to_bin(enc.ids, out_file)
    return total_tokens


def process_arrow_file(input_path: Path, output_path: Path, column: str) -> Dict[str, object]:
    total_tokens = 0
    mmap = pa.memory_map(str(input_path), "r")
    reader = None
    try:
        try:
            reader = ipc.open_file(mmap)
            batches = (reader.get_batch(i) for i in range(reader.num_record_batches))
        except Exception:
            reader = ipc.open_stream(mmap)
            batches = reader

        with output_path.open("wb") as f_out:
            for batch in batches:
                if column not in batch.schema.names:
                    raise RuntimeError(f"column '{column}' not found in {input_path}")
                arr = batch.column(batch.schema.get_field_index(column))
                texts = texts_from_arrow_array(arr)
                total_tokens += encode_texts(texts, f_out)
            f_out.flush()
            os.fsync(f_out.fileno())
    finally:
        if reader is not None:
            try:
                reader.close()
            except Exception:
                pass
        try:
            mmap.close()
        except Exception:
            pass

    return {
        "input_bytes": input_path.stat().st_size,
        "output_bytes": output_path.stat().st_size,
        "tokens": total_tokens,
    }


def process_text_file(input_path: Path, output_path: Path) -> Dict[str, object]:
    global TOKENIZER
    if TOKENIZER is None:
        raise RuntimeError("tokenizer is not initialized")

    text = input_path.read_text(encoding="utf-8")
    enc = TOKENIZER.encode(text)
    with output_path.open("wb") as f_out:
        token_count = pack_ids_to_bin(enc.ids, f_out)
        f_out.flush()
        os.fsync(f_out.fileno())

    return {
        "input_bytes": input_path.stat().st_size,
        "output_bytes": output_path.stat().st_size,
        "tokens": token_count,
    }


def process_one_file(
    input_dir: str,
    output_dir: str,
    input_path: str,
    arrow_text_column: str,
) -> Dict[str, object]:
    path = Path(input_path)
    out_path = output_path_for(Path(input_dir), Path(output_dir), path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    start = time.perf_counter()
    if path.suffix.lower() == ".arrow":
        result = process_arrow_file(path, out_path, arrow_text_column)
    else:
        result = process_text_file(path, out_path)
    elapsed_s = time.perf_counter() - start

    return {
        "input_path": str(path),
        "output_path": str(out_path),
        "input_bytes": result["input_bytes"],
        "output_bytes": result["output_bytes"],
        "tokens": result["tokens"],
        "elapsed_s": elapsed_s,
    }


def stats_file_path(stats_dir: Path, run_id: int, kind: str, suffix: str) -> Path:
    if kind == "per_file":
        return stats_dir / f"run_{run_id:02d}_per_file{suffix}.csv"
    return stats_dir / f"run_{run_id:02d}_summary{suffix}.json"


def run_once(args: argparse.Namespace, input_files: List[Path]) -> Dict[str, object]:
    start = time.perf_counter()
    with ProcessPoolExecutor(
        max_workers=args.threads,
        initializer=init_worker,
        initargs=(str(args.model),),
    ) as executor:
        rows = list(
            executor.map(
                process_one_file,
                [str(args.input_dir)] * len(input_files),
                [str(args.output_dir)] * len(input_files),
                [str(path) for path in input_files],
                [args.arrow_text_column] * len(input_files),
                chunksize=1,
            )
        )
    elapsed_s = time.perf_counter() - start

    total_bytes = sum(int(row["input_bytes"]) for row in rows)
    total_tokens = sum(int(row["tokens"]) for row in rows)
    throughput_MBps = 0.0
    throughput_tokens_per_s = 0.0
    if elapsed_s > 0:
        throughput_MBps = (total_bytes / (1024.0 * 1024.0)) / elapsed_s
        throughput_tokens_per_s = total_tokens / elapsed_s

    return {
        "num_files": len(rows),
        "total_bytes": total_bytes,
        "total_tokens": total_tokens,
        "elapsed_s": elapsed_s,
        "throughput_MBps": throughput_MBps,
        "throughput_tokens_per_s": throughput_tokens_per_s,
        "rows": rows,
    }


def write_stats(stats_dir: Path, run_id: int, suffix: str, summary: Dict[str, object]) -> None:
    stats_dir.mkdir(parents=True, exist_ok=True)

    csv_path = stats_file_path(stats_dir, run_id, "per_file", suffix)
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "input_path",
                "output_path",
                "input_bytes",
                "output_bytes",
                "tokens",
                "elapsed_s",
            ],
        )
        writer.writeheader()
        for row in summary["rows"]:
            writer.writerow(row)

    json_path = stats_file_path(stats_dir, run_id, "summary", suffix)
    payload = dict(summary)
    payload.pop("rows", None)
    payload["per_file_csv"] = str(csv_path)
    json_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="CPU baseline tokenizer for Arrow/text inputs."
    )
    parser.add_argument("--backend", choices=("cpu",), default="cpu")
    parser.add_argument("--input-dir", type=Path, default=DEFAULT_INPUT_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--stats-dir", type=Path, default=DEFAULT_STATS_DIR)
    parser.add_argument("--result-suffix", default="")
    parser.add_argument("--arrow-text-column", default="text")
    parser.add_argument("--no-progress", action="store_true")
    parser.add_argument("--no-cold-cache", action="store_true")
    args = parser.parse_args()

    if args.threads < 1:
        raise SystemExit("threads must be >= 1")
    if args.repeat < 1:
        raise SystemExit("repeat must be >= 1")
    if not args.input_dir.exists():
        raise SystemExit(f"input dir not found: {args.input_dir}")
    if not args.model.exists():
        raise SystemExit(f"model not found: {args.model}")

    input_files = iter_input_files(args.input_dir)
    if not input_files:
        raise SystemExit(f"no input files found in {args.input_dir}")

    args.output_dir.mkdir(parents=True, exist_ok=True)

    print("experiment_condition:")
    print(f"  backend={args.backend}")
    print(f"  model={args.model}")
    print(f"  input_dir={args.input_dir}")
    print(f"  output_dir={args.output_dir}")
    print("  pipeline=read+tokenize+bin_write")
    print("  file_filter=['.arrow', '.text', '.txt']")
    print(f"  arrow_text_column={args.arrow_text_column}")
    print(f"  parallelism=file_level_processes({args.threads})")
    print(f"  cold_cache={not args.no_cold_cache}")
    print(f"  progress={'off' if args.no_progress else 'on'}")
    print(f"  repeat={args.repeat}")

    for run_id in range(1, args.repeat + 1):
        if not args.no_cold_cache:
            drop_linux_caches()
        summary = run_once(args, input_files)
        write_stats(args.stats_dir, run_id, args.result_suffix, summary)
        print(
            f"run={run_id} elapsed_s={summary['elapsed_s']:.3f} "
            f"throughput_MBps={summary['throughput_MBps']:.3f} "
            f"throughput_tokens_per_s={summary['throughput_tokens_per_s']:.3f}"
        )


if __name__ == "__main__":
    main()
