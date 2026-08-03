#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SPDK_DIR="${ROOT_DIR}/storage/spdk"
RUNTIME_DIR="${ROOT_DIR}/storage/runtime"
COMPUTE_DIR="${ROOT_DIR}/compute"
PATCH_FILE="${SCRIPT_DIR}/patches/spdk-ndt-bpe.patch"
VENV_DIR="${COMPUTE_DIR}/venv"
JOBS="${JOBS:-$(nproc)}"
APPLY_ONLY=0
SKIP_SPDK_BUILD=0

usage() {
    cat <<'EOF'
Usage: scripts/patch.sh [options]

Apply the NDT-BPE SPDK patch and build all three components:
  1. compute client/Python binding
  2. SPDK NVMe-oF target
  3. bpe_process storage runtime

Options:
  --apply-only        Apply/verify the SPDK patch without building
  --skip-spdk-build   Build compute and bpe_process only
  -h, --help          Show this help

Environment:
  JOBS=N              Parallel build jobs (default: nproc)
  PYTHON=python3      Python interpreter used to create compute/venv
EOF
}

while (($#)); do
    case "$1" in
        --apply-only) APPLY_ONLY=1 ;;
        --skip-spdk-build) SKIP_SPDK_BUILD=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

for tool in git make cmake "${PYTHON:-python3}"; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "[ERROR] required command not found: ${tool}" >&2
        exit 1
    }
done

[[ -f "${PATCH_FILE}" ]] || {
    echo "[ERROR] SPDK patch not found: ${PATCH_FILE}" >&2
    exit 1
}

echo "[1/5] Initializing submodules"
git -C "${ROOT_DIR}" submodule sync --recursive
git -C "${ROOT_DIR}" submodule update --init --recursive \
    storage/spdk \
    compute/third_party/liburing \
    compute/third_party/tokenizers-cpp

echo "[2/5] Applying NDT-BPE SPDK patch"
if git -C "${SPDK_DIR}" apply --check "${PATCH_FILE}"; then
    git -C "${SPDK_DIR}" apply "${PATCH_FILE}"
    echo "      patch applied"
elif git -C "${SPDK_DIR}" apply --reverse --check "${PATCH_FILE}"; then
    echo "      patch already applied"
else
    echo "[ERROR] SPDK tree has changes that conflict with ${PATCH_FILE}" >&2
    echo "        inspect with: git -C storage/spdk status --short" >&2
    exit 1
fi

if ((APPLY_ONLY)); then
    echo "[DONE] Patch is applied; builds were skipped."
    exit 0
fi

echo "[3/5] Building compute client and Python binding"
PYTHON_BIN="${PYTHON:-python3}"
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
    "${PYTHON_BIN}" -m venv "${VENV_DIR}"
fi
# shellcheck source=/dev/null
source "${VENV_DIR}/bin/activate"
python -m pip install --upgrade pip setuptools wheel pybind11 pyarrow
"${COMPUTE_DIR}/build_all.sh"

echo "[4/5] Building bpe_process runtime"
make -C "${RUNTIME_DIR}" -j"${JOBS}"

if ((SKIP_SPDK_BUILD)); then
    echo "[5/5] SPDK build skipped by request"
else
    echo "[5/5] Building patched SPDK NVMe-oF target"
    if [[ ! -f "${SPDK_DIR}/mk/config.mk" ]]; then
        (cd "${SPDK_DIR}" && ./configure --with-nvme-cuse)
    fi
    make -C "${SPDK_DIR}" -j"${JOBS}"
fi

cat <<EOF

[DONE] NDT-BPE build completed.
  compute module : ${COMPUTE_DIR}/venv
  SPDK target    : ${SPDK_DIR}/build/bin/nvmf_tgt
  BPE runtime    : ${RUNTIME_DIR}/bin/bpe_process_main2

Runtime model files are not downloaded or committed by this script.
EOF
