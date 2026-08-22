#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import signal
import socket
import threading
import time
from pathlib import Path


CHUNK_SIZE = 256 * 1024
stop_event = threading.Event()


class Counters:
    def __init__(self) -> None:
        self.tx = 0
        self.rx = 0
        self.lock = threading.Lock()

    def add_tx(self, count: int) -> None:
        with self.lock:
            self.tx += count

    def add_rx(self, count: int) -> None:
        with self.lock:
            self.rx += count


def install_signals() -> None:
    def stop(_signum, _frame) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)


def paced_send(sock: socket.socket, rate_bps: int, counters: Counters) -> None:
    payload = bytes(CHUNK_SIZE)
    started = time.perf_counter()
    sent = 0
    try:
        while not stop_event.is_set():
            remaining = rate_bps * (time.perf_counter() - started) - sent
            if remaining < CHUNK_SIZE:
                time.sleep(min(0.002, max(0.0001, (CHUNK_SIZE - remaining) / rate_bps)))
                continue
            sock.sendall(payload)
            sent += len(payload)
            counters.add_tx(len(payload))
    except (BrokenPipeError, ConnectionResetError, OSError):
        stop_event.set()


def receive(sock: socket.socket, counters: Counters) -> None:
    try:
        while not stop_event.is_set():
            data = sock.recv(CHUNK_SIZE)
            if not data:
                stop_event.set()
                break
            counters.add_rx(len(data))
    except (ConnectionResetError, OSError):
        stop_event.set()


def configure(sock: socket.socket) -> None:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


def connect_with_retry(host: str, port: int) -> socket.socket:
    deadline = time.monotonic() + 15
    while True:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        configure(sock)
        try:
            sock.connect((host, port))
            return sock
        except OSError:
            sock.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)


def run_client(host: str, port: int, rate_bps: int, counters: Counters) -> None:
    upload = connect_with_retry(host, port)
    upload.sendall(b"U")
    download = connect_with_retry(host, port)
    download.sendall(b"D")
    print("READY", flush=True)

    threads = [
        threading.Thread(target=paced_send, args=(upload, rate_bps, counters), daemon=True),
        threading.Thread(target=receive, args=(download, counters), daemon=True),
    ]
    for thread in threads:
        thread.start()
    while not stop_event.wait(0.2):
        pass
    for sock in (upload, download):
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()
    for thread in threads:
        thread.join(timeout=2)


def run_server(bind: str, port: int, rate_bps: int, counters: Counters) -> None:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind, port))
    listener.listen(4)
    connections: dict[bytes, socket.socket] = {}
    try:
        while len(connections) < 2 and not stop_event.is_set():
            sock, _ = listener.accept()
            configure(sock)
            mode = sock.recv(1)
            if mode not in {b"U", b"D"}:
                sock.close()
                continue
            connections[mode] = sock
        print("READY", flush=True)
        threads = [
            threading.Thread(target=receive, args=(connections[b"U"], counters), daemon=True),
            threading.Thread(target=paced_send, args=(connections[b"D"], rate_bps, counters), daemon=True),
        ]
        for thread in threads:
            thread.start()
        while not stop_event.wait(0.2):
            pass
        for sock in connections.values():
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        for thread in threads:
            thread.join(timeout=2)
    finally:
        listener.close()


def write_summary(path: Path, role: str, requested_rate: int, duration: float, counters: Counters) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=["role", "requested_MBps", "duration_s", "tx_bytes", "rx_bytes", "tx_MBps", "rx_MBps"],
        )
        writer.writeheader()
        writer.writerow(
            {
                "role": role,
                "requested_MBps": requested_rate / 1_000_000,
                "duration_s": duration,
                "tx_bytes": counters.tx,
                "rx_bytes": counters.rx,
                "tx_MBps": counters.tx / duration / 1_000_000 if duration else 0,
                "rx_MBps": counters.rx / duration / 1_000_000 if duration else 0,
            }
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=["server", "client"])
    parser.add_argument("--host", default="192.168.1.160")
    parser.add_argument("--bind", default="192.168.1.160")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--rate-mbps", type=float, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    install_signals()
    counters = Counters()
    rate_bps = int(args.rate_mbps * 1_000_000)
    started = time.perf_counter()
    try:
        if args.role == "server":
            run_server(args.bind, args.port, rate_bps, counters)
        else:
            run_client(args.host, args.port, rate_bps, counters)
    finally:
        write_summary(args.summary, args.role, rate_bps, time.perf_counter() - started, counters)


if __name__ == "__main__":
    main()
