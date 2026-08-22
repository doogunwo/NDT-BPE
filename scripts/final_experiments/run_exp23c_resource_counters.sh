#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
RUNNER="$REPO/scripts/final_experiments/run_ndt_slot_sweep_single_shard.py"
RESTART=/tmp/restart_ndt_stack_exp23.sh
MONITOR=/tmp/monitor_exp23c_storage.sh
ROOT=/mnt/nvme/final_experiments/exp23c_resource_counters_writeback_20260819
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
REPEATS=3

mkdir -p "$ROOT"
rm -f "$ROOT/COMPLETED" "$ROOT/FAILED"
trap 'touch "$ROOT/FAILED"' ERR

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
  return 1
}

restart_runtime() {
  local workers="$1"
  sync
  ssh Nudepc2 "bash '$RESTART' '$workers'"
  wait_for_stack
}

run_case() {
  local workers="$1" slots="$2" repeat="$3"
  local dir="$ROOT/workers_${workers}/slots_${slots}/repeat_${repeat}"
  local remote_mon="/tmp/exp23c_w${workers}_s${slots}_r${repeat}"
  local monitor_pid
  mkdir -p "$dir"
  if [[ -f "$dir/COMPLETED" ]]; then
    echo "CASE_SKIP workers=$workers slots=$slots repeat=$repeat"
    return
  fi
  if [[ ! -e "$dir/ndt_output.tokens" ]]; then
    sudo -n ln "$PREALLOCATED" "$dir/ndt_output.tokens"
    sudo -n chown doogunwo:doogunwo "$dir/ndt_output.tokens"
  fi

  cache_drop
  monitor_pid=$(ssh Nudepc2 "sudo rm -rf '$remote_mon'; sudo nohup bash '$MONITOR' '$remote_mon' >/tmp/exp23c_monitor_launcher.log 2>&1 & echo \$!")
  sleep 2
  sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
    "$PY" "$RUNNER" --run-dir "$dir" --slots "$slots" \
    --runtime-workers "$workers" > "$dir/foreground.log" 2>&1
  ssh Nudepc2 "sudo kill -TERM '$monitor_pid' 2>/dev/null || true"
  for _ in $(seq 1 50); do
    ssh Nudepc2 "sudo test -f '$remote_mon/DONE'" && break
    sleep 0.1
  done
  scp -qr Nudepc2:"$remote_mon" "$dir/storage_counters"
  touch "$dir/COMPLETED"
}

echo "EXP23C_START $(date --iso-8601=seconds)" | tee "$ROOT/master.log"
index=0
for workers in 4 6 8 12 16; do
  restart_runtime "$workers"
  if [[ "$workers" -eq 8 || "$workers" -eq 12 || "$workers" -eq 16 ]]; then
    slots_list=(8 16)
  else
    slots_list=(8)
  fi
  for repeat in $(seq 1 "$REPEATS"); do
    for slots in "${slots_list[@]}"; do
      index=$((index + 1))
      echo "CASE_START index=$index/24 workers=$workers slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
      run_case "$workers" "$slots" "$repeat"
      echo "CASE_DONE index=$index/24 workers=$workers slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
    done
  done
done
touch "$ROOT/COMPLETED"
rm -f "$ROOT/FAILED"
echo "EXP23C_COMPLETE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
