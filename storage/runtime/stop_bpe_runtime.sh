#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  exec sudo -n "$0" "$@"
fi

SESSION_NAME="${1:-runtime-codex}"
tmux kill-session -t "${SESSION_NAME}" >/dev/null 2>&1 || true
pkill -f bpe_process_main2 >/dev/null 2>&1 || true
