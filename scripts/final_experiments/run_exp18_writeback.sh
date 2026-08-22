#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
ROOT=/mnt/nvme/final_experiments/exp18_writeback_20260818
INPUT_DIR="$ROOT/input"
RAY_OUT="$ROOT/ray_output"
RAY_STATS="$ROOT/ray_stats"
NDT_OUT_DIR="$ROOT/ndt_output"
CANONICAL_INPUT=/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow
PREALLOCATED_NDT_OUTPUT=/mnt/nvme/writeback_validation_20260818/shard0.tokens
SPDK_LOG=/tmp/nvmf-exp23.log
RUNTIME_SESSION=runtime-exp23

mkdir -p "$INPUT_DIR" "$RAY_OUT" "$RAY_STATS" "$NDT_OUT_DIR"
ln -f "$CANONICAL_INPUT" "$INPUT_DIR/data-00000-of-00080.arrow"
if [[ ! -e "$NDT_OUT_DIR/shard0.tokens" ]]; then
    sudo -n ln "$PREALLOCATED_NDT_OUTPUT" "$NDT_OUT_DIR/shard0.tokens"
    sudo -n chown doogunwo:doogunwo "$NDT_OUT_DIR/shard0.tokens"
fi

ssh Nudepc2 "sudo tmux set-option -t '$RUNTIME_SESSION' history-limit 20000"

sudo -n sync
echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
ray_spdk_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
ip -s -j link show enp5s0 > "$ROOT/ray_nic_before.json"

cd "$REPO"
sudo -n "$PY" exp/exp_13/test2.py \
    --input-dir "$INPUT_DIR" \
    --output-dir "$RAY_OUT" \
    --dispatchers 1 \
    --task-cpus 1 \
    --dispatcher-cpus 0 \
    --ray-num-cpus 1 \
    --repeat 1 \
    --stats-dir "$RAY_STATS" \
    --result-suffix "_exp18_writeback_20260818" \
    --no-cold-cache \
    --no-progress \
    2>&1 | tee "$ROOT/ray_stdout.log"

ip -s -j link show enp5s0 > "$ROOT/ray_nic_after.json"
ray_spdk_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
ray_spdk_count=$((ray_spdk_end - ray_spdk_start))
ssh Nudepc2 "sudo tail -n +$((ray_spdk_start + 1)) '$SPDK_LOG' | head -n '$ray_spdk_count'" \
    > "$ROOT/ray_spdk.log"

sudo -n sync
echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
ssh Nudepc2 "sudo tmux clear-history -t '$RUNTIME_SESSION'"
ndt_spdk_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
ip -s -j link show enp5s0 > "$ROOT/ndt_nic_before.json"

sudo -n env \
    LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
    "$PY" "$REPO/scripts/final_experiments/run_ndt_exp18_writeback.py" \
    2>&1 | tee "$ROOT/ndt_stdout.log"

ip -s -j link show enp5s0 > "$ROOT/ndt_nic_after.json"
ndt_spdk_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
ndt_spdk_count=$((ndt_spdk_end - ndt_spdk_start))
ssh Nudepc2 "sudo tail -n +$((ndt_spdk_start + 1)) '$SPDK_LOG' | head -n '$ndt_spdk_count'" \
    > "$ROOT/ndt_spdk.log"
ssh Nudepc2 "sudo tmux capture-pane -p -t '$RUNTIME_SESSION' -S -20000" \
    > "$ROOT/ndt_runtime.log"

touch "$ROOT/COMPLETED"
echo "EXP18_COMPLETE root=$ROOT"
