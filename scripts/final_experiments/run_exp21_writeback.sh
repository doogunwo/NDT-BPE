#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
ROOT=/mnt/nvme/final_experiments/exp21_writeback_20260818
INPUT=/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
SPDK_LOG=/tmp/nvmf-exp23.log
RUNTIME_SESSION=runtime-exp23

mkdir -p "$ROOT/input"
ln -f "$INPUT" "$ROOT/input/data-00000-of-00080.arrow"
rm -f "$ROOT/COMPLETED" "$ROOT/FAILED"

cache_drop() {
    sudo -n sync
    echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
}

run_ray() {
    local n="$1" run="$ROOT/run_$(printf '%02d' "$1")" suffix="_exp21_writeback_run$(printf '%02d' "$1")_20260818"
    mkdir -p "$run/ray_output" "$run/ray_stats"
    cache_drop
    local log_start log_end log_count
    log_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    ip -s -j link show enp5s0 > "$run/ray_nic_before.json"
    sudo -n "$PY" "$REPO/exp/exp_13/test2.py" \
        --input-dir "$ROOT/input" --output-dir "$run/ray_output" \
        --dispatchers 1 --task-cpus 1 --dispatcher-cpus 0 --ray-num-cpus 1 \
        --repeat 1 --stats-dir "$run/ray_stats" --result-suffix "$suffix" \
        --no-cold-cache --no-progress 2>&1 | tee "$run/ray_stdout.log"
    ip -s -j link show enp5s0 > "$run/ray_nic_after.json"
    log_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    log_count=$((log_end - log_start))
    ssh Nudepc2 "sudo tail -n +$((log_start + 1)) '$SPDK_LOG' | head -n '$log_count'" > "$run/ray_spdk.log"
}

run_ndt() {
    local n="$1" run="$ROOT/run_$(printf '%02d' "$1")" runtime_log="/tmp/exp21_runtime_run$(printf '%02d' "$1").log"
    mkdir -p "$run"
    if [[ ! -e "$run/ndt_output.tokens" ]]; then
        sudo -n ln "$PREALLOCATED" "$run/ndt_output.tokens"
        sudo -n chown doogunwo:doogunwo "$run/ndt_output.tokens"
    fi
    ssh Nudepc2 "sudo install -m 0666 /dev/null '$runtime_log'"
    ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION' 'cat >> $runtime_log'"
    cache_drop
    local log_start log_end log_count
    log_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    ip -s -j link show enp5s0 > "$run/ndt_nic_before.json"
    sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
        "$PY" "$REPO/scripts/final_experiments/run_ndt_exp21.py" --run-dir "$run" \
        2>&1 | tee "$run/ndt_stdout.log"
    ip -s -j link show enp5s0 > "$run/ndt_nic_after.json"
    log_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    log_count=$((log_end - log_start))
    ssh Nudepc2 "sudo tail -n +$((log_start + 1)) '$SPDK_LOG' | head -n '$log_count'" > "$run/ndt_spdk.log"
    ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION'"
    scp Nudepc2:"$runtime_log" "$run/ndt_runtime.log"
}

trap 'ssh Nudepc2 "sudo tmux pipe-pane -t runtime-exp23" >/dev/null 2>&1 || true; touch "$ROOT/FAILED"' ERR
echo "EXP21_START $(date --iso-8601=seconds)" | tee "$ROOT/master.log"
for run in 1 2 3; do
    echo "RUN_${run}_START $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
    if (( run % 2 == 1 )); then
        run_ray "$run"
        run_ndt "$run"
    else
        run_ndt "$run"
        run_ray "$run"
    fi
    echo "RUN_${run}_DONE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
done
"$PY" "$REPO/scripts/final_experiments/aggregate_exp21_writeback.py" "$ROOT" | tee "$ROOT/aggregate_stdout.log"
touch "$ROOT/COMPLETED"
rm -f "$ROOT/FAILED"
echo "EXP21_COMPLETE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
