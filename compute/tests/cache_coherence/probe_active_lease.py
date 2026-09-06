#!/usr/bin/env python3
import argparse
import errno
import os
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--expect", choices=("active", "released"), default="active")
    args = parser.parse_args()

    deadline = time.monotonic() + args.timeout
    attempts = 0
    while time.monotonic() < deadline:
        attempts += 1
        try:
            fd = os.open(args.path, os.O_WRONLY | os.O_CLOEXEC)
        except OSError as exc:
            if exc.errno == errno.ETXTBSY:
                if args.expect == "active":
                    print(f"PASS: active lease rejected writable open after {attempts} attempts")
                    return 0
                time.sleep(0.1)
                continue
            if exc.errno != errno.ENOENT:
                raise
        else:
            os.close(fd)
            if args.expect == "released":
                print(f"PASS: writable open succeeded after lease release ({attempts} attempts)")
                return 0
        if args.expect == "released":
            time.sleep(0.1)
            continue
        time.sleep(0.1)
    if args.expect == "active":
        raise TimeoutError("did not observe ETXTBSY while the NDT output lease was active")
    raise TimeoutError("writable open remained denied after lease completion")


if __name__ == "__main__":
    raise SystemExit(main())
