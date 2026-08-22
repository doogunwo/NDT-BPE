#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
RUNNER="$REPO/scripts/final_experiments/run_ndt_slot_sweep_single_shard.py"
RESTART=/tmp/restart_ndt_stack_exp23.sh
ROOT=/mnt/nvme/final_experiments/exp23b_worker_slot_matrix_writeback_20260819
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
RUNTIME_SESSION=runtime-exp23
WORKERS=(4 6 8 10 12 14 16)
SLOTS=(1 2 4 8 16)
REPEATS=3
pipe_active=0

mkdir -p "$ROOT"
rm -f "$ROOT/COMPLETED" "$ROOT/FAILED"

stop_pipe() {
  if [[ "$pipe_active" -eq 1 ]]; then
    ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION'" >/dev/null 2>&1 || true
    pipe_active=0
  fi
}

fail() {
  stop_pipe
  touch "$ROOT/FAILED"
}

trap fail ERR
trap stop_pipe EXIT

cache_drop() {
  sudo -n sync
  echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
}

wait_for_stack() {
  for _ in $(seq 1 120); do
    if [[ -c /dev/ng2n1 && -r /mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow ]]; then
      return 0
    fi
    sleep 0.5
  done
  echo "NVMe-oF path did not recover" >&2
  return 1
}

restart_runtime() {
  local workers="$1"
  stop_pipe
  sync
  ssh Nudepc2 "bash '$RESTART' '$workers'"
  wait_for_stack
}

run_case() {
  local workers="$1" slots="$2" repeat="$3"
  local dir="$ROOT/workers_${workers}/slots_${slots}/repeat_${repeat}"
  local runtime_log="/tmp/exp23b_w${workers}_s${slots}_r${repeat}.log"
  mkdir -p "$dir"

  if [[ -f "$dir/COMPLETED" ]]; then
    echo "CASE_SKIP workers=$workers slots=$slots repeat=$repeat"
    return
  fi

  if [[ ! -e "$dir/ndt_output.tokens" ]]; then
    sudo -n ln "$PREALLOCATED" "$dir/ndt_output.tokens"
    sudo -n chown doogunwo:doogunwo "$dir/ndt_output.tokens"
  fi

  ssh Nudepc2 "sudo install -m 0666 /dev/null '$runtime_log'"
  ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION' 'cat >> $runtime_log'"
  pipe_active=1
  cache_drop

  "$PY" - <<'PY' > "$dir/nic_before.json"
import json
from pathlib import Path
print(json.dumps({k:int(Path('/sys/class/net/enp5s0/statistics/'+k).read_text()) for k in ('rx_bytes','tx_bytes')}))
PY

  sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
    "$PY" "$RUNNER" --run-dir "$dir" --slots "$slots" \
    --runtime-workers "$workers" > "$dir/foreground.log" 2>&1

  "$PY" - <<'PY' > "$dir/nic_after.json"
import json
from pathlib import Path
print(json.dumps({k:int(Path('/sys/class/net/enp5s0/statistics/'+k).read_text()) for k in ('rx_bytes','tx_bytes')}))
PY

  stop_pipe
  scp -q Nudepc2:"$runtime_log" "$dir/runtime.log"
  touch "$dir/COMPLETED"
}

total=$((${#WORKERS[@]} * ${#SLOTS[@]} * REPEATS))
index=0
echo "EXP23B_MATRIX_START $(date --iso-8601=seconds) total=$total" | tee "$ROOT/master.log"

for workers in "${WORKERS[@]}"; do
  echo "WORKER_START workers=$workers $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
  restart_runtime "$workers"
  for repeat in $(seq 1 "$REPEATS"); do
    if (( repeat % 2 == 1 )); then
      order=(1 2 4 8 16)
    else
      order=(16 8 4 2 1)
    fi
    for slots in "${order[@]}"; do
      index=$((index + 1))
      echo "CASE_START index=$index/$total workers=$workers slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
      run_case "$workers" "$slots" "$repeat"
      echo "CASE_DONE index=$index/$total workers=$workers slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
    done
  done
  echo "WORKER_DONE workers=$workers $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
done

touch "$ROOT/COMPLETED"
rm -f "$ROOT/FAILED"
echo "EXP23B_MATRIX_COMPLETE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
