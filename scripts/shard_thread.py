# shard_Thread.py
import argparse
import os
import time
from dataclasses import dataclass
from queue import Queue
from threading import Thread, Lock, current_thread
from typing import Optional, Iterable

import numpy as np
import pyarrow as pa
import pyarrow.ipc as ipc
from tokenizers import Tokenizer

# data-00000-of-00080.arrow
prefix = "data-"
suffix = "-of-00080.arrow"

folder_path = "/mnt/nvme/openwebtext_for_ndt"
write_path = "/mnt/nvme/bin"


def zfill5(n: int) -> str:
    return str(n).zfill(5)


@dataclass
class ProfileStats:
    thread_name: str
    shards_done: int = 0
    tokens: int = 0

    # Read breakdown
    read_batch_s: float = 0.0     # iterating/obtaining record batch object
    decode_s: float = 0.0         # batch column extract + to_pylist + filter

    # Compute breakdown
    tokenize_s: float = 0.0       # tokenizer.encode
    pack_s: float = 0.0           # pack/convert ids -> bytes

    # IO breakdown
    write_sys_s: float = 0.0      # f_out.write(buf)
    flush_s: float = 0.0          # f_out.flush()
    fsync_s: float = 0.0          # os.fsync()

    @property
    def total_s(self) -> float:
        return (
            self.read_batch_s
            + self.decode_s
            + self.tokenize_s
            + self.pack_s
            + self.write_sys_s
            + self.flush_s
            + self.fsync_s
        )

    def throughput_tokens_per_hour(self) -> float:
        if self.total_s <= 0:
            return 0.0
        return self.tokens / self.total_s * 3600.0


class ShardTokenizerWorker:
    """
    Streaming pipeline per shard:
      batch_read -> decode -> tokenize -> pack -> write(syscall) -> (flush) -> (fsync)

    Notes on measurement:
      - read_batch_s measures overhead of advancing the batch iterator / fetching the batch.
        For file-mode generator this includes get_batch(i) call; for stream-mode it's next(reader).
      - decode_s measures column extraction + to_pylist + filtering.
      - pack_s isolates ids->bytes packing (CPU/memory copies) so IO is "write_sys_s/flush_s/fsync_s".
      - write is done once per batch (one syscall per batch) to reduce syscall-count noise.
    """

    def __init__(
        self,
        task_q: Queue,
        folder_path: str,
        write_path: str,
        results: list[ProfileStats],
        results_lock: Lock,
        io_lock: Optional[Lock] = None,
        tokenizer_name: str = "gpt2",
    ):
        self.task_q = task_q
        self.folder_path = folder_path
        self.write_path = write_path
        self.io_lock = io_lock
        self.results = results
        self.results_lock = results_lock

        # Thread-local tokenizer instance
        self.tokenizer = Tokenizer.from_pretrained(tokenizer_name)
        self.stats = ProfileStats(thread_name="(init)")

    def run(self):
        self.stats.thread_name = current_thread().name

        while True:
            shard_n = self.task_q.get()
            if shard_n is None:
                self.task_q.task_done()
                break
            try:
                self.process_shard_streaming(shard_n)
            finally:
                self.task_q.task_done()

        with self.results_lock:
            self.results.append(self.stats)

    def _batch_iterator(self, reader, mode: str) -> Iterable:
        if mode == "stream":
            # Iterator yields batches
            return reader
        # file mode: generator over indices
        return (reader.get_batch(i) for i in range(reader.num_record_batches))

    def process_shard_streaming(self, shard_n: int):
        shard_id = zfill5(shard_n)
        file_name = f"{prefix}{shard_id}{suffix}"
        input_path = os.path.join(self.folder_path, file_name)

        if self.io_lock:
            with self.io_lock:
                os.makedirs(self.write_path, exist_ok=True)
        else:
            os.makedirs(self.write_path, exist_ok=True)

        out_path = os.path.join(self.write_path, f"{shard_id}.bin")

        f = pa.memory_map(input_path, "r")
        reader = None
        mode = None

        try:
            try:
                reader = ipc.open_stream(f)
                mode = "stream"
            except Exception:
                reader = ipc.open_file(f)
                mode = "file"

            col_name = "text"
            batches_iter = self._batch_iterator(reader, mode)

            with open(out_path, "wb") as f_out:
                it = iter(batches_iter)

                while True:
                    # ---- batch read timing (advance iterator / fetch batch) ----
                    t_rb = time.perf_counter()
                    try:
                        batch = next(it)
                    except StopIteration:
                        self.stats.read_batch_s += time.perf_counter() - t_rb
                        break
                    self.stats.read_batch_s += time.perf_counter() - t_rb

                    # ---- decode timing (column extract + to_pylist + filter) ----
                    t_dec = time.perf_counter()

                    if col_name not in batch.schema.names:
                        raise RuntimeError(
                            f"Column '{col_name}' not found in {input_path}. cols={batch.schema.names}"
                        )

                    arr = batch.column(batch.schema.get_field_index(col_name))
                    part = arr.to_pylist()
                    texts = [s for s in part if s]

                    self.stats.decode_s += time.perf_counter() - t_dec
                    if not texts:
                        continue

                    # ---- tokenize timing ----
                    t_tok = time.perf_counter()
                    ids_batch = [self.tokenizer.encode(s).ids for s in texts]
                    self.stats.tokenize_s += time.perf_counter() - t_tok

                    # token count
                    self.stats.tokens += sum(len(ids) for ids in ids_batch)

                    # ---- pack timing (ids -> uint32 bytes). pack to ONE buffer per batch ----
                    t_pack = time.perf_counter()
                    # Flatten without constructing huge Python list if possible:
                    # - fromiter consumes a generator, but needs total length unknown; it's OK.
                    flat = np.fromiter(
                        (t for ids in ids_batch for t in ids),
                        dtype=np.uint32,
                    )
                    buf = flat.tobytes()
                    self.stats.pack_s += time.perf_counter() - t_pack

                    if not buf:
                        continue

                    # ---- write syscall timing ----
                    t_ws = time.perf_counter()
                    f_out.write(buf)
                    self.stats.write_sys_s += time.perf_counter() - t_ws

                # ---- flush timing ----
                t_fl = time.perf_counter()
                f_out.flush()
                self.stats.flush_s += time.perf_counter() - t_fl

                # ---- fsync timing ----
                t_fs = time.perf_counter()
                os.fsync(f_out.fileno())
                self.stats.fsync_s += time.perf_counter() - t_fs

            self.stats.shards_done += 1

        finally:
            try:
                if reader is not None:
                    reader.close()
            except Exception:
                pass
            try:
                f.close()
            except Exception:
                pass


def main() -> None:
    parser = argparse.ArgumentParser(
        description="N threads tokenize shards 0..N-1 with detailed profiling: "
        "batch_read, decode, tokenize, pack, write_syscall, flush, fsync"
    )
    parser.add_argument("threads", type=int, help="스레드 수(=처리할 샤드 수). 예: 2면 shard 0,1 처리")
    parser.add_argument("--max-shards", type=int, default=80, help="전체 샤드 개수(기본 80)")
    parser.add_argument("--tokenizer", type=str, default="gpt2", help="tokenizers pretrained name (default: gpt2)")
    args = parser.parse_args()

    n_threads = args.threads
    if n_threads <= 0:
        raise ValueError("threads는 1 이상이어야 합니다.")
    if n_threads > args.max_shards:
        raise ValueError(f"threads는 1~{args.max_shards} 범위여야 합니다.")

    q: Queue = Queue()

    # tasks: shard 0..n_threads-1
    for shard_n in range(n_threads):
        q.put(shard_n)

    # sentinel for shutdown
    for _ in range(n_threads):
        q.put(None)

    io_lock = Lock()
    results_lock = Lock()
    results: list[ProfileStats] = []

    threads: list[Thread] = []
    for i in range(n_threads):
        worker = ShardTokenizerWorker(
            q,
            folder_path,
            write_path,
            results=results,
            results_lock=results_lock,
            io_lock=io_lock,
            tokenizer_name=args.tokenizer,
        )
        t = Thread(target=worker.run, name=f"shard-worker-{i}", daemon=False)
        t.start()
        threads.append(t)

    q.join()
    for t in threads:
        t.join()

    results_sorted = sorted(results, key=lambda s: s.thread_name)

    def fmt(s: float) -> str:
        return f"{s:.3f}s"

    print("\n=== Per-thread profiling ===")
    print("Fields: shards, tokens, batch_read, decode, tokenize, pack, write_syscall, flush, fsync, total, throughput(tokens/hour)\n")

    for s in results_sorted:
        print(
            f"[{s.thread_name}] shards={s.shards_done} tokens={s.tokens} "
            f"batch_read={fmt(s.read_batch_s)} decode={fmt(s.decode_s)} "
            f"tokenize={fmt(s.tokenize_s)} pack={fmt(s.pack_s)} "
            f"write_syscall={fmt(s.write_sys_s)} flush={fmt(s.flush_s)} fsync={fmt(s.fsync_s)} "
            f"total={fmt(s.total_s)} throughput={s.throughput_tokens_per_hour():.2f}"
        )

    # Aggregate (sum of per-thread stage times)
    agg = ProfileStats(thread_name="ALL")
    for s in results_sorted:
        agg.shards_done += s.shards_done
        agg.tokens += s.tokens
        agg.read_batch_s += s.read_batch_s
        agg.decode_s += s.decode_s
        agg.tokenize_s += s.tokenize_s
        agg.pack_s += s.pack_s
        agg.write_sys_s += s.write_sys_s
        agg.flush_s += s.flush_s
        agg.fsync_s += s.fsync_s

    print(
        f"\n[ALL] shards={agg.shards_done} tokens={agg.tokens} "
        f"batch_read={fmt(agg.read_batch_s)} decode={fmt(agg.decode_s)} "
        f"tokenize={fmt(agg.tokenize_s)} pack={fmt(agg.pack_s)} "
        f"write_syscall={fmt(agg.write_sys_s)} flush={fmt(agg.flush_s)} fsync={fmt(agg.fsync_s)} "
        f"total={fmt(agg.total_s)} throughput={agg.throughput_tokens_per_hour():.2f}"
    )


if __name__ == "__main__":
    main()
