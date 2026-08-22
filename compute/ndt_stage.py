#!/usr/bin/env python3
"""Manage and consume persistent NDT-BPE Arrow metadata indexes."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import ndt_compute as nc


def print_stats(stats) -> None:
    for key, value in stats.items():
        print(f"{key}={value}")


def add_stage_selection(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("input", help="Arrow IPC input file")
    parser.add_argument(
        "--stage-path",
        "--index-path",
        default="",
        help="Persistent metadata-index path (default: input-derived .ndtidx path)",
    )
    parser.add_argument("--batch-start", type=int, default=-1)
    parser.add_argument("--batch-count", type=int, default=-1)
    parser.add_argument("--max-extents", type=int, default=128)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Prepare, validate, and consume an NDT-BPE metadata-only Arrow index."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    prepare = sub.add_parser("prepare", help="Create or reuse a persistent metadata index")
    add_stage_selection(prepare)
    prepare.add_argument("--force", action="store_true", help="Rebuild even if valid")
    prepare.add_argument("--verbose", action="store_true")

    inspect = sub.add_parser("inspect", help="Validate a persistent metadata index")
    add_stage_selection(inspect)

    run = sub.add_parser("run", help="Run NDT-BPE with an explicit stage policy")
    add_stage_selection(run)
    run.add_argument("--device", default="/dev/nvme0n1")
    run.add_argument("--output", required=True)
    run.add_argument(
        "--stage-mode",
        choices=("auto", "require-prestaged", "rebuild"),
        default="require-prestaged",
    )
    run.add_argument("--opcode", type=lambda value: int(value, 0), default=0xD4)
    run.add_argument("--nsid", type=int, default=1)
    run.add_argument("--queue-depth", type=int, default=64)
    run.add_argument("--max-inflight", type=int, default=0)
    run.add_argument("--slots", type=int, default=16)
    run.add_argument("--fixed-slot", type=int, default=-1)
    run.add_argument("--completion-timeout-us", type=int, default=30_000_000)
    run.add_argument("--admin", action="store_true")
    run.add_argument("--verbose", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    input_path = str(Path(args.input).resolve())
    stage_path = str(Path(args.stage_path).resolve()) if args.stage_path else ""

    if args.command == "prepare":
        result = nc.prepare_arrow_stage(
            input_path=input_path,
            stage_path=stage_path,
            force_rebuild=args.force,
            max_extents=args.max_extents,
            arrow_batch_start=args.batch_start,
            arrow_batch_count=args.batch_count,
            verbose=args.verbose,
        )
    elif args.command == "inspect":
        result = nc.inspect_arrow_stage(
            input_path=input_path,
            stage_path=stage_path,
            max_extents=args.max_extents,
            arrow_batch_start=args.batch_start,
            arrow_batch_count=args.batch_count,
        )
        if not result["valid"]:
            print_stats(result)
            return 2
    else:
        result = nc.tokenize_to_nvme(
            dev_path=args.device,
            input_path=input_path,
            output_path=str(Path(args.output).resolve()),
            opcode=args.opcode,
            nsid=args.nsid,
            queue_depth=args.queue_depth,
            max_inflight=args.max_inflight,
            slots=args.slots,
            fixed_slot=args.fixed_slot,
            max_extents=args.max_extents,
            arrow_batch_start=args.batch_start,
            arrow_batch_count=args.batch_count,
            completion_timeout_us=args.completion_timeout_us,
            admin=args.admin,
            verbose=args.verbose,
            stage_path=stage_path,
            stage_mode=args.stage_mode,
        )

    print_stats(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
