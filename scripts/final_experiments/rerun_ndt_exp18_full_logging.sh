#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
ROOT=/mnt/nvme/final_experiments/exp18_writeback_20260818
SPDK_LOG=/tmp/nvmf-exp23.log
RUNTIME_LOG=/tmp/exp18_runtime_full.log
RUNTIME_SESSION=runtime-exp23

for file in ndt_result.csv ndt_stdout.log ndt_spdk.log ndt_runtime.log \
            ndt_nic_before.json ndt_nic_after.json; do
    if [[ -e "$ROOT/$file" && ! -e "$ROOT/${file}.discard_incomplete_runtime_log" ]]; then
        mv "$ROOT/$file" "$ROOT/${file}.discard_incomplete_runtime_log"
    fi
done
rm -f "$ROOT/COMPLETED"

ssh Nudepc2 "sudo install -m 0666 /dev/null '$RUNTIME_LOG'"
ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION' 'cat >> $RUNTIME_LOG'"

cleanup() {
    ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION'" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sudo -n sync
echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
ndt_spdk_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
ip -s -j link show enp5s0 > "$ROOT/ndt_nic_before.json"

cd "$REPO"
sudo -n env \
    LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
    "$PY" "$REPO/scripts/final_experiments/run_ndt_exp18_writeback.py" \
    2>&1 | tee "$ROOT/ndt_stdout.log"

ip -s -j link show enp5s0 > "$ROOT/ndt_nic_after.json"
ndt_spdk_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
ndt_spdk_count=$((ndt_spdk_end - ndt_spdk_start))
ssh Nudepc2 "sudo tail -n +$((ndt_spdk_start + 1)) '$SPDK_LOG' | head -n '$ndt_spdk_count'" \
    > "$ROOT/ndt_spdk.log"

cleanup
trap - EXIT
scp Nudepc2:"$RUNTIME_LOG" "$ROOT/ndt_runtime.log"
touch "$ROOT/COMPLETED"
echo "EXP18_NDT_RERUN_COMPLETE root=$ROOT"
