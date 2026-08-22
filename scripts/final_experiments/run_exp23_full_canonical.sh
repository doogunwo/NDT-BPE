#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
INPUT=/mnt/nvme/openwebtext_for_ndt
INDEX=/mnt/nvme/openwebtext_for_ndt_indices
DEVICE=/dev/ng2n1
RESTART=/tmp/restart_ndt_stack_exp23.sh
NDT_RUNNER="$REPO/scripts/final_experiments/run_ndt_exp23.py"
RAY_RUNNER="$REPO/exp/exp_13/test2.py"
REMOTE_ROOT=/mnt/nvme/final_experiments/exp23_full_writeback_20260820
NDT_OUTPUT="$REMOTE_ROOT/reusable_ndt_output"
RAY_OUTPUT="$REMOTE_ROOT/reusable_ray_output"
LOCAL_ROOT=/mnt/local_nvme/final_experiments/exp23_full_writeback_20260820
PILOT="$LOCAL_ROOT/pilot_8x8"
RESULT="$LOCAL_ROOT/canonical"
RAY_VALUES=(1 2 4 8 16)
NDT_CONFIGS=(2:2 4:4 6:8 8:8 16:16)
REPEATS=1
monitor_active=0

mkdir -p "$RESULT" "$NDT_OUTPUT" "$RAY_OUTPUT"
rm -f "$RESULT/COMPLETED" "$RESULT/FAILED"

snapshot_compute_nic() {
  local out="$1" iface
  iface=$(ip route get 192.168.1.160 | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}' | head -1)
  printf 'iface=%s\nrx_bytes=%s\ntx_bytes=%s\n' "$iface" \
    "$(cat /sys/class/net/$iface/statistics/rx_bytes)" \
    "$(cat /sys/class/net/$iface/statistics/tx_bytes)" > "$out"
}

snapshot_storage_nic() {
  local out="$1"
  ssh Nudepc2 'iface=$(ip route get 192.168.1.110 | awk '\''{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}'\'' | head -1); printf "iface=%s\nrx_bytes=%s\ntx_bytes=%s\n" "$iface" "$(cat /sys/class/net/$iface/statistics/rx_bytes)" "$(cat /sys/class/net/$iface/statistics/tx_bytes)"' > "$out"
}

start_monitors() {
  local dir="$1"
  mkdir -p "$dir"
  snapshot_compute_nic "$dir/compute_nic_before.txt"
  snapshot_storage_nic "$dir/storage_nic_before.txt"
  mpstat -P ALL 5 > "$dir/compute_mpstat.log" & MON_CPU=$!
  iostat -x 5 > "$dir/compute_iostat.log" & MON_IO=$!
  ssh Nudepc2 'mpstat -P ALL 5' > "$dir/storage_mpstat.log" & MON_SCPU=$!
  monitor_active=1
}

stop_monitors() {
  local dir="$1"
  if [[ "$monitor_active" -eq 1 ]]; then
    kill "$MON_CPU" "$MON_IO" "$MON_SCPU" 2>/dev/null || true
    wait "$MON_CPU" "$MON_IO" "$MON_SCPU" 2>/dev/null || true
    monitor_active=0
  fi
  snapshot_compute_nic "$dir/compute_nic_after.txt"
  snapshot_storage_nic "$dir/storage_nic_after.txt"
}

fail() {
  if [[ "$monitor_active" -eq 1 ]]; then
    kill "$MON_CPU" "$MON_IO" "$MON_SCPU" 2>/dev/null || true
  fi
  touch "$RESULT/FAILED"
}
trap fail ERR

wait_for_storage() {
  for _ in $(seq 1 120); do
    [[ -c "$DEVICE" && -r "$INPUT/data-00000-of-00080.arrow" ]] && return 0
    sleep 0.5
  done
  return 1
}

restart_ndt() {
  local workers="$1"
  ssh Nudepc2 "bash '$RESTART' '$workers'"
  wait_for_storage
}

run_ray() {
  local workers="$1" repeat="$2" dir="$RESULT/ray/workers_${1}/repeat_${2}"
  if [[ -f "$dir/run_01_summary.json" ]]; then
    "$PY" - "$dir/run_01_summary.json" <<'PY'
import json, sys
row=json.load(open(sys.argv[1]))
assert row["total_errors"] == 0 and row["num_files"] == 80 and row["total_tokens"] > 0
PY
    touch "$dir/COMPLETED"
    echo "SKIP Ray workers=$workers repeat=$repeat"
    return
  fi
  mkdir -p "$dir"
  echo "CASE_START Ray workers=$workers repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
  start_monitors "$dir"
  sudo -n "$PY" "$RAY_RUNNER" \
    --input-dir "$INPUT" --output-dir "$RAY_OUTPUT" \
    --dispatchers "$workers" --task-cpus 1 --dispatcher-cpus 0 \
    --ray-num-cpus "$workers" --repeat 1 --stats-dir "$dir" \
    --cold-cache --no-progress > "$dir/foreground.log" 2>&1
  stop_monitors "$dir"
  "$PY" - "$dir/run_01_summary.json" <<'PY'
import json, sys
p=sys.argv[1]; row=json.load(open(p))
assert row["total_errors"] == 0 and row["num_files"] == 80 and row["total_tokens"] > 0
PY
  touch "$dir/COMPLETED"
  echo "CASE_DONE Ray workers=$workers repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
}

run_ndt() {
  local workers="$1" slots="$2" repeat="$3"
  local dir="$RESULT/ndt/workers_${workers}_slots_${slots}/repeat_${repeat}"
  if [[ -f "$dir/run_$(printf '%02d' "$repeat")_summary.json" ]]; then
    echo "SKIP NDT workers=$workers slots=$slots repeat=$repeat"
    return
  fi
  mkdir -p "$dir"
  restart_ndt "$workers"
  echo "CASE_START NDT workers=$workers slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
  start_monitors "$dir"
  sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
    "$PY" "$NDT_RUNNER" \
    --device "$DEVICE" --input-dir "$INPUT" --index-dir "$INDEX" \
    --output-dir "$NDT_OUTPUT" --stats-dir "$dir" \
    --condition "full_workers${workers}_slots${slots}" --run "$repeat" \
    --slots "$slots" --max-inflight "$slots" --runtime-workers "$workers" \
    --limit 80 --cold-cache > "$dir/foreground.log" 2>&1
  stop_monitors "$dir"
  "$PY" - "$dir/run_$(printf '%02d' "$repeat")_summary.json" <<'PY'
import json, sys
row=json.load(open(sys.argv[1]))
assert row["num_files"] == 80 and row["total_errors"] == 0 and row["total_tokens"] > 0
PY
  touch "$dir/COMPLETED"
  echo "CASE_DONE NDT workers=$workers slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
}

echo "WAIT_PILOT $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
while [[ ! -f "$PILOT/COMPLETED" ]]; do
  if [[ -f "$PILOT/FAILED" ]]; then
    echo "pilot failed" >&2
    exit 1
  fi
  sleep 30
done

pilot_dst="$RESULT/ndt/workers_8_slots_8/repeat_1"
mkdir -p "$pilot_dst"
cp "$PILOT/run_01_summary.json" "$PILOT/run_01_per_file.csv" "$pilot_dst/"
touch "$pilot_dst/COMPLETED"
echo "PILOT_IMPORTED NDT workers=8 slots=8 repeat=1 $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"

for repeat in 1 2 3; do
  if (( repeat % 2 == 1 )); then
    ray_order=(1 2 4 8 16)
    ndt_order=(2:2 4:4 6:8 8:8 16:16)
  else
    ray_order=(16 8 4 2 1)
    ndt_order=(16:16 8:8 6:8 4:4 2:2)
  fi
  for i in 0 1 2 3 4; do
    run_ray "${ray_order[$i]}" "$repeat"
    IFS=: read -r workers slots <<< "${ndt_order[$i]}"
    run_ndt "$workers" "$slots" "$repeat"
  done
done

touch "$RESULT/COMPLETED"
rm -f "$RESULT/FAILED"
echo "EXP23_FULL_COMPLETE $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
