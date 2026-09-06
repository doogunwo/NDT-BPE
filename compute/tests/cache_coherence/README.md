# NDT output cache-coherence prototype

This directory reproduces the NDT-BPE write-back hazard without touching the
real NVMe namespace.  The test creates a temporary ext4 filesystem on a loop
device, caches a file page, changes the file's physical block through the loop
block device, and then asks a small kernel ioctl module to invalidate the
corresponding inode/file-offset range.

Expected sequence:

1. A buffered file read returns pattern `A`.
2. A raw block-device write changes the physical extent to pattern `B`.
3. A second buffered read still returns stale pattern `A`.
4. `NDT_CACHE_COMPLETE_RANGE` invalidates the inode page-cache range.
5. A final buffered read returns pattern `B`.

Run on the compute host:

```bash
make
sudo ./run_loopback_test.sh
```

The production protocol keeps one `/dev/ndt_cache` control FD open from
`NDT_CACHE_BEGIN_RANGE` until `NDT_CACHE_COMPLETE_RANGE`.  BEGIN rejects an
existing writable FD or writable mmap and denies new writable opens.  COMPLETE
invalidates the output range and releases the write denial.  Closing the
control FD without COMPLETE performs best-effort invalidation and automatic
lease release.  Reads and read-only mmap remain allowed; this is a write/layout
lease rather than a universal read-blocking file lease.
