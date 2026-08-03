#!/usr/bin/env python3
import os
from pathlib import Path
from setuptools import setup

try:
    from pybind11.setup_helpers import Pybind11Extension, build_ext
except ImportError as exc:
    raise SystemExit(
        "pybind11 is required to build this module. "
        "Install it with: pip install pybind11"
    ) from exc

ROOT = Path(__file__).resolve().parent

liburing_a = ROOT / "third_party" / "liburing" / "src" / "liburing.a"
if not liburing_a.exists():
    raise SystemExit(
        f"Missing {liburing_a}. Run ./build_compute.sh in {ROOT} first."
    )

default_pyarrow_root = ROOT.parent / "scripts" / ".venv" / "lib" / "python3.12" / "site-packages" / "pyarrow"
pyarrow_root = Path(os.environ.get("PYARROW_ROOT", str(default_pyarrow_root)))
arrow_include = pyarrow_root / "include"

def find_libarrow(root: Path) -> Path | None:
    for name in ("libarrow.so.2300", "libarrow.so"):
        candidate = root / name
        if candidate.exists():
            return candidate
    matches = sorted(root.glob("libarrow.so.*"))
    return matches[0] if matches else None

arrow_lib = find_libarrow(pyarrow_root)
if arrow_lib is None:
    raise SystemExit(
        "Missing libarrow.so in pyarrow. "
        "Set PYARROW_ROOT or install pyarrow in scripts/.venv."
    )

ext_modules = [
    Pybind11Extension(
        "ndt_compute",
        sources=[
            str(ROOT / "src" / "bindings.cpp"),
            str(ROOT / "src" / "arrow_text_dump_lib.cpp"),
            str(ROOT / "src" / "extent-index.cpp"),
            str(ROOT / "src" / "fallocate.cpp"),
            str(ROOT / "src" / "io-uring.cpp"),
            str(ROOT / "src" / "fiemap_schedule.cpp"),
        ],
        include_dirs=[
            str(ROOT / "include"),
            str(ROOT / "third_party" / "liburing" / "src" / "include"),
            str(arrow_include),
        ],
        extra_objects=[str(liburing_a), str(arrow_lib)],
        extra_compile_args=["-std=c++17"],
        extra_link_args=["-lpthread", "-ldl", "-lrt", f"-Wl,-rpath,{pyarrow_root}"],
    )
]

setup(
    name="ndt_compute",
    version="0.1.0",
    description="NDT-BPE compute bindings (FIEMAP + NVMe io_uring).",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
)
