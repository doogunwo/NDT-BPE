#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
ROOT=/mnt/nvme/final_experiments/exp22_nvmeof_writeback_20260818
INPUT=/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
BG_DIR=/mnt/nvme/exp22_nvmeof_background
BG_READ="$BG_DIR/read_source.bin"
BG_WRITE="$BG_DIR/write_target.bin"
SPDK_LOG=/tmp/nvmf-exp23.log
RUNTIME_SESSION=runtime-exp23

mkdir -p "$ROOT/input" "$ROOT/ray_output_scratch"
ln -f "$INPUT" "$ROOT/input/data-00000-of-00080.arrow"
[[ -f "$BG_READ" && -f "$BG_WRITE" ]] || { echo "background files are not prepared" >&2; exit 1; }
[[ "$(stat -c %s "$BG_READ")" -ge 4294967296 ]] || { echo "read background file is too small" >&2; exit 1; }
[[ "$(stat -c %s "$BG_WRITE")" -ge 4294967296 ]] || { echo "write background file is too small" >&2; exit 1; }
rm -f "$ROOT/COMPLETED" "$ROOT/FAILED"

fio_pid=""
pipe_active=0

stop_fio() {
    if [[ -n "$fio_pid" ]]; then
        kill -INT "$fio_pid" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "$fio_pid" 2>/dev/null || break
            sleep 0.2
        done
        kill -TERM "$fio_pid" 2>/dev/null || true
        wait "$fio_pid" 2>/dev/null || true
        fio_pid=""
    fi
}

stop_pipe() {
    if [[ "$pipe_active" -eq 1 ]]; then
        ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION'" >/dev/null 2>&1 || true
        pipe_active=0
    fi
}

fail() {
    stop_pipe
    stop_fio
    touch "$ROOT/FAILED"
}
trap fail ERR
trap 'stop_pipe; stop_fio' EXIT

cache_drop() {
    sudo -n sync
    echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null
}

snapshot_nic() {
    local label="$1" out="$2"
    printf '%s,%s,%s,%s,%s\n' "$label" \
        "$(cat /sys/class/net/enp5s0/statistics/rx_bytes)" \
        "$(cat /sys/class/net/enp5s0/statistics/tx_bytes)" \
        "$(ssh Nudepc2 'cat /sys/class/net/enp5s0/statistics/rx_bytes')" \
        "$(ssh Nudepc2 'cat /sys/class/net/enp5s0/statistics/tx_bytes')" >> "$out"
}

start_fio() {
    local rate="$1" case_dir="$2"
    if [[ "$rate" -eq 0 ]]; then
        printf '{"requested_rate_Bps":0,"jobs":[]}\n' > "$case_dir/fio.json"
        return
    fi
    local rate_bps=$((rate * 1000000))
    fio \
        --name=background-read --filename="$BG_READ" --rw=read --direct=1 \
        --ioengine=libaio --bs=128k --iodepth=32 --size=4G --time_based=1 --runtime=1200 \
        --rate="$rate_bps" --rate_ignore_thinktime=1 --invalidate=1 --fadvise_hint=0 \
        --name=background-write --filename="$BG_WRITE" --rw=write --direct=1 \
        --ioengine=libaio --bs=128k --iodepth=32 --size=4G --time_based=1 --runtime=1200 \
        --rate="$rate_bps" --rate_ignore_thinktime=1 --invalidate=1 --fadvise_hint=0 \
        --eta=never --output-format=json --output="$case_dir/fio.json" \
        > "$case_dir/fio_stdout.log" 2> "$case_dir/fio_stderr.log" &
    fio_pid=$!
    sleep 4
    kill -0 "$fio_pid"
}

finish_fio() {
    stop_fio
    [[ -s "$1/fio.json" ]] || { echo "missing fio JSON in $1" >&2; return 1; }
}

run_ray() {
    local rate="$1" repeat="$2" case_dir="$3"
    mkdir -p "$case_dir/ray_stats"
    cache_drop
    start_fio "$rate" "$case_dir"
    local log_start log_end log_count suffix
    log_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    printf 'label,compute_rx_bytes,compute_tx_bytes,storage_rx_bytes,storage_tx_bytes\n' > "$case_dir/nic_counters.csv"
    snapshot_nic before "$case_dir/nic_counters.csv"
    suffix="_exp22_nvmeof_rate${rate}_repeat${repeat}_20260818"
    sudo -n "$PY" "$REPO/exp/exp_13/test2.py" \
        --input-dir "$ROOT/input" --output-dir "$ROOT/ray_output_scratch" \
        --dispatchers 1 --task-cpus 1 --dispatcher-cpus 0 --ray-num-cpus 1 \
        --repeat 1 --stats-dir "$case_dir/ray_stats" --result-suffix "$suffix" \
        --no-cold-cache --no-progress > "$case_dir/foreground.log" 2>&1
    snapshot_nic after "$case_dir/nic_counters.csv"
    log_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    log_count=$((log_end-log_start))
    ssh Nudepc2 "sudo tail -n +$((log_start+1)) '$SPDK_LOG' | head -n '$log_count'" > "$case_dir/spdk.log"
    finish_fio "$case_dir"
}

run_ndt() {
    local rate="$1" repeat="$2" case_dir="$3" runtime_log
    if [[ ! -e "$case_dir/ndt_output.tokens" ]]; then
        sudo -n ln "$PREALLOCATED" "$case_dir/ndt_output.tokens"
        sudo -n chown doogunwo:doogunwo "$case_dir/ndt_output.tokens"
    fi
    runtime_log="/tmp/exp22_nvmeof_runtime_rate${rate}_repeat${repeat}.log"
    ssh Nudepc2 "sudo install -m 0666 /dev/null '$runtime_log'"
    ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION' 'cat >> $runtime_log'"
    pipe_active=1
    cache_drop
    start_fio "$rate" "$case_dir"
    local log_start log_end log_count
    log_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    printf 'label,compute_rx_bytes,compute_tx_bytes,storage_rx_bytes,storage_tx_bytes\n' > "$case_dir/nic_counters.csv"
    snapshot_nic before "$case_dir/nic_counters.csv"
    sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
        "$PY" "$REPO/scripts/final_experiments/run_ndt_exp21.py" --run-dir "$case_dir" \
        > "$case_dir/foreground.log" 2>&1
    snapshot_nic after "$case_dir/nic_counters.csv"
    log_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    log_count=$((log_end-log_start))
    ssh Nudepc2 "sudo tail -n +$((log_start+1)) '$SPDK_LOG' | head -n '$log_count'" > "$case_dir/spdk.log"
    stop_pipe
    scp -q Nudepc2:"$runtime_log" "$case_dir/runtime.log"
    finish_fio "$case_dir"
}

echo "EXP22_NVMEOF_START $(date --iso-8601=seconds)" | tee "$ROOT/master.log"
case_index=0
for rate in 0 30 60 90; do
    for repeat in 1 2 3; do
        if (( repeat % 2 == 1 )); then systems=(ray-only ndt-bpe); else systems=(ndt-bpe ray-only); fi
        for system in "${systems[@]}"; do
            case_index=$((case_index+1))
            case_dir="$ROOT/rate_${rate}/repeat_${repeat}/${system}"
            mkdir -p "$case_dir"
            echo "CASE_START index=$case_index/24 rate=$rate repeat=$repeat system=$system $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
            if [[ "$system" == "ray-only" ]]; then run_ray "$rate" "$repeat" "$case_dir"; else run_ndt "$rate" "$repeat" "$case_dir"; fi
            touch "$case_dir/COMPLETED"
            echo "CASE_DONE index=$case_index/24 rate=$rate repeat=$repeat system=$system $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
        done
    done
done
touch "$ROOT/COMPLETED"
rm -f "$ROOT/FAILED"
echo "EXP22_NVMEOF_COMPLETE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
