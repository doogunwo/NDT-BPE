#!/usr/bin/env bash
set -euo pipefail

REPO=/home/doogunwo/Desktop/test/NDT-BPE
PY="$REPO/compute/venv/bin/python"
RUNNER="$REPO/scripts/final_experiments/run_ndt_exp23.py"
RESTART=/tmp/restart_ndt_stack_exp23.sh
INPUT=/mnt/nvme/openwebtext_for_ndt
INDEX=/mnt/nvme/openwebtext_for_ndt_indices
PREALLOCATED=/mnt/nvme/writeback_validation_20260818/shard0.tokens
OUTPUT=/mnt/nvme/final_experiments/exp23_full_writeback_20260820/reusable_ndt_output
RESULT=/mnt/local_nvme/final_experiments/exp23_full_writeback_20260820/pilot_8x8
DEVICE=/dev/ng2n1

mkdir -p "$OUTPUT" "$RESULT"
rm -f "$RESULT/COMPLETED" "$RESULT/FAILED"
trap 'touch "$RESULT/FAILED"' ERR

mapfile -t inputs < <(find "$INPUT" -maxdepth 1 -type f -name 'data-*-of-00080.arrow' | sort)
[[ "${#inputs[@]}" -eq 80 ]]
for input_path in "${inputs[@]}"; do
  stem=$(basename "$input_path" .arrow)
  [[ -f "$INDEX/$stem.ndtidx" ]]
  output_path="$OUTPUT/$stem.bin"
  if [[ ! -e "$output_path" ]]; then
    sudo -n ln "$PREALLOCATED" "$output_path"
    sudo -n chown doogunwo:doogunwo "$output_path"
  fi
done

ssh Nudepc2 "bash '$RESTART' 8"
for _ in $(seq 1 120); do
  [[ -c "$DEVICE" && -r "$INPUT/data-00000-of-00080.arrow" ]] && break
  sleep 0.5
done
[[ -c "$DEVICE" && -r "$INPUT/data-00000-of-00080.arrow" ]]

echo "PILOT_START $(date --iso-8601=seconds) workers=8 slots=8" | tee "$RESULT/master.log"
sudo -n env LD_LIBRARY_PATH="$REPO/compute/venv/lib/python3.12/site-packages/pyarrow" \
  "$PY" "$RUNNER" \
  --device "$DEVICE" --input-dir "$INPUT" --index-dir "$INDEX" \
  --output-dir "$OUTPUT" --stats-dir "$RESULT" \
  --condition full_pilot_workers8_slots8 --run 1 \
  --slots 8 --max-inflight 8 --runtime-workers 8 \
  --limit 80 --cold-cache 2>&1 | tee -a "$RESULT/master.log"

touch "$RESULT/COMPLETED"
rm -f "$RESULT/FAILED"
echo "PILOT_COMPLETE $(date --iso-8601=seconds)" | tee -a "$RESULT/master.log"
