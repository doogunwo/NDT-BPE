#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  exec sudo -n "$0" "$@"
fi

SESSION_NAME="${1:-monitor-codex}"
INTERVAL_MS="${2:-1000}"
CPU_CORES="${3:-0-11}"
NET_IFACE="${4:-enp5s0}"
BLOCK_DEV="${5:-nvme0n1}"
CSV_PATH="${6:-/tmp/bpe_monitor_qos.csv}"
LOG_PATH="${7:-/tmp/${SESSION_NAME}.log}"
ROOT_DIR="/home/doogunwo/NDT-BPE/storage/runtime"

cd "${ROOT_DIR}"
if tmux has-session -t "${SESSION_NAME}" 2>/dev/null; then
  tmux kill-session -t "${SESSION_NAME}"
fi

tmux new-session -d -s "${SESSION_NAME}" "cd ${ROOT_DIR} && ./bin/bpe_monitor --interval-ms ${INTERVAL_MS} --cpu-cores ${CPU_CORES} --net-iface ${NET_IFACE} --block-dev ${BLOCK_DEV} --csv-path ${CSV_PATH} 2>&1 | tee ${LOG_PATH}; exec bash"
echo "session=${SESSION_NAME} csv=${CSV_PATH} log=${LOG_PATH}"
