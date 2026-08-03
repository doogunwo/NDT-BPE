#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_FILE="${NDT_BPE_ENV:-$ROOT_DIR/.env}"
if [[ -f "${ENV_FILE}" ]]; then
  set -a
  # shellcheck source=/dev/null
  source "${ENV_FILE}"
  set +a
else
  echo "[FATAL] env file not found: ${ENV_FILE}"
  exit 1
fi

: "${TARGET_PORT:?Missing TARGET_PORT}"
: "${NQN:?Missing NQN}"
: "${MNT:?Missing MNT}"
: "${DEV:?Missing DEV}"
if [[ -z "${STORAGE_IP-}" && -z "${STORAGE_IPS-}" ]]; then
  echo "[FATAL] Missing STORAGE_IP or STORAGE_IPS"
  exit 1
fi

echo "[INFO] Unmount ${MNT} if mounted..."
if mountPOINT=$(mount | grep "${MNT}"); then
    sudo umount "${MNT}"
fi

echo "[INFO] Disconnect all NVMe-oF..."
sudo nvme disconnect-all || true

IPS=()
if [[ -n "${STORAGE_IPS-}" ]]; then
  read -r -a IPS <<< "${STORAGE_IPS}"
else
  IPS=("${STORAGE_IP}")
fi

connected=0
for ip in "${IPS[@]}"; do
  echo "[INFO] Discover on ${ip}:${TARGET_PORT} ..."
  if sudo nvme discover -t tcp -a "${ip}" -s "${TARGET_PORT}"; then
    echo "[INFO] Connect NQN=${NQN} ..."
    if sudo nvme connect -t tcp -a "${ip}" -s "${TARGET_PORT}" -n "${NQN}"; then
      STORAGE_IP="${ip}"
      connected=1
      break
    fi
  fi
done

if [[ "${connected}" -ne 1 ]]; then
  echo "[FATAL] Failed to connect to any target in TARGET_IPS"
  exit 1
fi

# [추가] 커널이 장치 노드(/dev/nvme0n1)를 생성할 때까지 대기 (Race Condition 방지)
echo "[INFO] Waiting for device ${DEV} to appear..."
sleep 2

detect_spdk_dev() {
  sudo nvme list | awk '/SPDK bdev Controller/ {print $1; exit}'
}

ACTUAL_DEV="$(detect_spdk_dev || true)"
if [[ -z "${ACTUAL_DEV}" ]]; then
  ACTUAL_DEV="${DEV}"
fi

# 장치가 실제로 생겼는지 확인
if [ ! -b "${ACTUAL_DEV}" ]; then
    echo "[FATAL] Device ${ACTUAL_DEV} not found after connection!"
    exit 1
fi

echo "[INFO] Using device: ${ACTUAL_DEV}"
sudo mkdir -p "${MNT}"

# 파일시스템 확인 후 없으면 포맷 (ext4)
FSTYPE="$(lsblk -no FSTYPE "${ACTUAL_DEV}" | head -n1 || true)"
if [ -z "${FSTYPE}" ]; then
  echo "[WARN] No filesystem on ${ACTUAL_DEV}. Formatting ext4..."
  sudo mkfs.ext4 -F "${ACTUAL_DEV}"
fi

echo "[INFO] Mount ${ACTUAL_DEV} -> ${MNT}"
sudo mount "${ACTUAL_DEV}" "${MNT}"

# 권한 조정 (현재 사용자에게 쓰기 권한 부여)
echo "[INFO] Chown ${MNT} to user..."
sudo chown "$(whoami):$(whoami)" "${MNT}"

if [ -f "./wiki_corpus.txt" ]; then
  echo "[INFO] Copying corpus to remote storage..."
  cp -f ./wiki_corpus.txt "${MNT}/"
fi

echo "[SUCCESS] Connected + mounted: ${ACTUAL_DEV} -> ${MNT}"
