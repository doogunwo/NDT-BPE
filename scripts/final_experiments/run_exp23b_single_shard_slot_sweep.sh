#!/usr/bin/env bash
set -euo pipefail
REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
ROOT=/mnt/nvme/final_experiments/exp23b_single_shard_slots_writeback_20260818
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
SPDK_LOG=/tmp/nvmf-exp23.log
RUNTIME_SESSION=runtime-exp23
pipe_active=0

mkdir -p "$ROOT"; rm -f "$ROOT/COMPLETED" "$ROOT/FAILED"
stop_pipe(){ if [[ "$pipe_active" -eq 1 ]]; then ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION'" >/dev/null 2>&1 || true; pipe_active=0; fi; }
fail(){ stop_pipe; touch "$ROOT/FAILED"; }
trap fail ERR; trap stop_pipe EXIT
cache_drop(){ sudo -n sync; echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null; }

run_case(){
  local slots="$1" repeat="$2" dir="$ROOT/slots_${1}/repeat_${2}" runtime_log
  mkdir -p "$dir"
  if [[ ! -e "$dir/ndt_output.tokens" ]]; then
    sudo -n ln "$PREALLOCATED" "$dir/ndt_output.tokens"
    sudo -n chown doogunwo:doogunwo "$dir/ndt_output.tokens"
  fi
  runtime_log="/tmp/exp23b_slots${slots}_repeat${repeat}.log"
  ssh Nudepc2 "sudo install -m 0666 /dev/null '$runtime_log'"
  ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION' 'cat >> $runtime_log'"; pipe_active=1
  cache_drop
  local start end count
  start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
  "$PY" - <<'PY' > "$dir/nic_before.json"
import json
from pathlib import Path
print(json.dumps({k:int(Path('/sys/class/net/enp5s0/statistics/'+k).read_text()) for k in ('rx_bytes','tx_bytes')}))
PY
  sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
    "$PY" "$REPO/scripts/final_experiments/run_ndt_slot_sweep_single_shard.py" \
    --run-dir "$dir" --slots "$slots" > "$dir/foreground.log" 2>&1
  "$PY" - <<'PY' > "$dir/nic_after.json"
import json
from pathlib import Path
print(json.dumps({k:int(Path('/sys/class/net/enp5s0/statistics/'+k).read_text()) for k in ('rx_bytes','tx_bytes')}))
PY
  end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'"); count=$((end-start))
  ssh Nudepc2 "sudo tail -n +$((start+1)) '$SPDK_LOG' | head -n '$count'" > "$dir/spdk.log"
  stop_pipe; scp -q Nudepc2:"$runtime_log" "$dir/runtime.log"
  touch "$dir/COMPLETED"
}

echo "EXP23B_SINGLE_START $(date --iso-8601=seconds) runtime_workers=2" | tee "$ROOT/master.log"
index=0
for repeat in 1 2 3; do
  if (( repeat % 2 == 1 )); then order=(1 2 4 8 16); else order=(16 8 4 2 1); fi
  for slots in "${order[@]}"; do
    index=$((index+1)); echo "CASE_START index=$index/15 slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
    run_case "$slots" "$repeat"
    echo "CASE_DONE index=$index/15 slots=$slots repeat=$repeat $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
  done
done
touch "$ROOT/COMPLETED"; rm -f "$ROOT/FAILED"
echo "EXP23B_SINGLE_COMPLETE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
