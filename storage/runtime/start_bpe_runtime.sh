#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  exec sudo -n "$0" "$@"
fi

SESSION_NAME="${1:-runtime-codex}"
MODE="${2:-arrow}"
EXEC_MODE="${3:-process}"
WORKERS="${4:-16}"
LOG_PATH="${5:-/tmp/${SESSION_NAME}.log}"
ROOT_DIR="/home/doogunwo/NDT-BPE/storage/runtime"

cd "${ROOT_DIR}"
if tmux has-session -t "${SESSION_NAME}" 2>/dev/null; then
  tmux kill-session -t "${SESSION_NAME}"
fi

tmux new-session -d -s "${SESSION_NAME}" "cd ${ROOT_DIR} && ./bin/bpe_process_main2 --mode=${MODE} --exec-mode=${EXEC_MODE} --workers=${WORKERS} 2>&1 | tee ${LOG_PATH}; exec bash"
echo "session=${SESSION_NAME} log=${LOG_PATH} mode=${MODE} exec=${EXEC_MODE} workers=${WORKERS}"
