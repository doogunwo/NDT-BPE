#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test_root=$(mktemp -d --tmpdir ndt-cache-test.XXXXXX)
image_path="$test_root/ext4.img"
mount_path="$test_root/mnt"
loop_device=""

cleanup() {
    set +e
    if mountpoint -q "$mount_path"; then
        umount "$mount_path"
    fi
    if [[ -n "$loop_device" ]]; then
        losetup -d "$loop_device"
    fi
    if lsmod | awk '{print $1}' | grep -qx ndt_cache; then
        rmmod ndt_cache
    fi
    rm -rf -- "$test_root"
}
trap cleanup EXIT

if [[ $(id -u) -ne 0 ]]; then
    echo "run with sudo: sudo $0" >&2
    exit 2
fi

make -C "$script_dir"
if lsmod | awk '{print $1}' | grep -qx ndt_cache; then
    rmmod ndt_cache
fi
insmod "$script_dir/ndt_cache.ko"

truncate -s 128M "$image_path"
mkfs.ext4 -q -F "$image_path"
loop_device=$(losetup --find --show "$image_path")
mkdir -p "$mount_path"
mount -t ext4 "$loop_device" "$mount_path"

"$script_dir/raw_extent_test" \
    "$mount_path/output.bin" "$loop_device" /dev/ndt_cache
