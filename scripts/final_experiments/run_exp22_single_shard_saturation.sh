#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
ROOT=/mnt/nvme/final_experiments/exp22_writeback_20260818
INPUT=/mnt/nvme/openwebtext_for_ndt/data-00000-of-00080.arrow
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
LOAD_SCRIPT="$REPO/scripts/final_experiments/network_load.py"
REMOTE_LOAD_SCRIPT=/tmp/exp22_network_load.py
SPDK_LOG=/tmp/nvmf-exp23.log
RUNTIME_SESSION=runtime-exp23

mkdir -p "$ROOT/input" "$ROOT/ray_output_scratch"
ln -f "$INPUT" "$ROOT/input/data-00000-of-00080.arrow"
rm -f "$ROOT/COMPLETED" "$ROOT/FAILED"

load_pid=""
server_pid=""
pipe_active=0

stop_load() {
    if [[ -n "$load_pid" ]]; then
        kill -INT "$load_pid" 2>/dev/null || true
        for _ in $(seq 1 40); do
            kill -0 "$load_pid" 2>/dev/null || break
            sleep 0.2
        done
        kill -TERM "$load_pid" 2>/dev/null || true
        wait "$load_pid" 2>/dev/null || true
        load_pid=""
    fi
    if [[ -n "$server_pid" ]]; then
        ssh Nudepc2 "kill -TERM '$server_pid' 2>/dev/null || true" || true
        server_pid=""
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
    stop_load
    touch "$ROOT/FAILED"
}
trap fail ERR
trap 'stop_pipe; stop_load' EXIT

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

start_load() {
    local rate="$1" port="$2" case_dir="$3"
    if [[ "$rate" -eq 0 ]]; then
        printf 'role,requested_MBps,duration_s,tx_bytes,rx_bytes,tx_MBps,rx_MBps\nclient,0,0,0,0,0,0\n' > "$case_dir/background_client.csv"
        printf 'role,requested_MBps,duration_s,tx_bytes,rx_bytes,tx_MBps,rx_MBps\nserver,0,0,0,0,0,0\n' > "$case_dir/background_server.csv"
        return
    fi
    server_pid=$(ssh Nudepc2 "rm -f /tmp/exp22_server_${port}.csv /tmp/exp22_server_${port}.log; nohup python3 '$REMOTE_LOAD_SCRIPT' server --port '$port' --rate-mbps '$rate' --summary /tmp/exp22_server_${port}.csv >/tmp/exp22_server_${port}.log 2>&1 & echo \$!")
    sleep 0.5
    python3 "$LOAD_SCRIPT" client --port "$port" --rate-mbps "$rate" \
        --summary "$case_dir/background_client.csv" > "$case_dir/background_client.log" 2>&1 &
    load_pid=$!
    for _ in $(seq 1 120); do
        if grep -q '^READY' "$case_dir/background_client.log" 2>/dev/null; then
            sleep 1
            return
        fi
        kill -0 "$load_pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

collect_server() {
    local rate="$1" port="$2" case_dir="$3"
    [[ "$rate" -eq 0 ]] && return
    for _ in $(seq 1 40); do
        ssh Nudepc2 "test -s /tmp/exp22_server_${port}.csv" && break
        sleep 0.2
    done
    scp -q Nudepc2:"/tmp/exp22_server_${port}.csv" "$case_dir/background_server.csv"
    scp -q Nudepc2:"/tmp/exp22_server_${port}.log" "$case_dir/background_server.log"
}

run_ray() {
    local rate="$1" repeat="$2" port="$3" case_dir="$4"
    mkdir -p "$case_dir/ray_stats"
    start_load "$rate" "$port" "$case_dir"
    cache_drop
    local log_start log_end log_count suffix
    log_start=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    printf 'label,compute_rx_bytes,compute_tx_bytes,storage_rx_bytes,storage_tx_bytes\n' > "$case_dir/nic_counters.csv"
    snapshot_nic before "$case_dir/nic_counters.csv"
    suffix="_exp22_rate${rate}_repeat${repeat}_20260818"
    sudo -n "$PY" "$REPO/exp/exp_13/test2.py" \
        --input-dir "$ROOT/input" --output-dir "$ROOT/ray_output_scratch" \
        --dispatchers 1 --task-cpus 1 --dispatcher-cpus 0 --ray-num-cpus 1 \
        --repeat 1 --stats-dir "$case_dir/ray_stats" --result-suffix "$suffix" \
        --no-cold-cache --no-progress > "$case_dir/foreground.log" 2>&1
    snapshot_nic after "$case_dir/nic_counters.csv"
    log_end=$(ssh Nudepc2 "sudo wc -l < '$SPDK_LOG'")
    log_count=$((log_end-log_start))
    ssh Nudepc2 "sudo tail -n +$((log_start+1)) '$SPDK_LOG' | head -n '$log_count'" > "$case_dir/spdk.log"
    stop_load
    collect_server "$rate" "$port" "$case_dir"
}

run_ndt() {
    local rate="$1" repeat="$2" port="$3" case_dir="$4" runtime_log
    if [[ ! -e "$case_dir/ndt_output.tokens" ]]; then
        sudo -n ln "$PREALLOCATED" "$case_dir/ndt_output.tokens"
        sudo -n chown doogunwo:doogunwo "$case_dir/ndt_output.tokens"
    fi
    runtime_log="/tmp/exp22_runtime_rate${rate}_repeat${repeat}.log"
    ssh Nudepc2 "sudo install -m 0666 /dev/null '$runtime_log'"
    ssh Nudepc2 "sudo tmux pipe-pane -t '$RUNTIME_SESSION' 'cat >> $runtime_log'"
    pipe_active=1
    start_load "$rate" "$port" "$case_dir"
    cache_drop
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
    stop_load
    collect_server "$rate" "$port" "$case_dir"
}

echo "EXP22_START $(date --iso-8601=seconds)" | tee "$ROOT/master.log"
case_index=0
for rate in 0 30 60 90; do
    for repeat in 1 2 3; do
        if (( repeat % 2 == 1 )); then systems=(ray-only ndt-bpe); else systems=(ndt-bpe ray-only); fi
        for system in "${systems[@]}"; do
            case_index=$((case_index+1))
            case_dir="$ROOT/rate_${rate}/repeat_${repeat}/${system}"
            mkdir -p "$case_dir"
            port=$((53000 + case_index))
            echo "CASE_START index=$case_index/24 rate=$rate repeat=$repeat system=$system $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
            if [[ "$system" == "ray-only" ]]; then run_ray "$rate" "$repeat" "$port" "$case_dir"; else run_ndt "$rate" "$repeat" "$port" "$case_dir"; fi
            touch "$case_dir/COMPLETED"
            echo "CASE_DONE index=$case_index/24 rate=$rate repeat=$repeat system=$system $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
        done
    done
done
touch "$ROOT/COMPLETED"
rm -f "$ROOT/FAILED"
echo "EXP22_COMPLETE $(date --iso-8601=seconds)" | tee -a "$ROOT/master.log"
