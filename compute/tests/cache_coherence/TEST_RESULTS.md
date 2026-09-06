# Cache-coherence prototype results

Tested on the compute host on 2026-08-24.

## Environment

- Kernel: Ubuntu `6.8.0-106-generic`
- Test filesystem: temporary 128 MiB ext4 loopback image
- Production smoke input: one 503,327,778-byte canonical Arrow shard
- Production mode: `NDT_CACHE_COHERENCE_STRICT=1`

## Loopback raw-block reproduction

The raw write addressed only the physical extent of a newly created 4 KiB
test file in the temporary loop filesystem.

```text
OBSERVED stale buffered page after raw physical-block write
PASS: BEGIN rejected an existing writable FD
PASS: BEGIN rejected an existing writable mmap
PASS: global registry rejected a second lease on the same inode
PASS: active lease denied a new writable open
PASS: active lease denied truncate
PASS: COMPLETE invalidated cache populated during the lease
PASS: BEGIN removed dirty host state before raw pattern F
PASS: control-FD release automatically released the lease
```

This establishes both hazards and the intended behavior:

- BEGIN rejects preexisting writers and writable mappings, and the module-wide
  inode registry permits only one NDT lease per output file.
- While the control FD remains open, new writable opens and truncate fail with
  `ETXTBSY`; operations that require a new writable FD, including hole punch
  and fallocate-based layout changes, are therefore excluded as well.
- COMPLETE removes cache populated during storage-side writes, releases write
  denial, and permits new writers again.
- Closing the control FD without COMPLETE exercises the same `.release()` path
  used after process termination and automatically releases the lease.
- The complete loopback suite was repeated five additional times; all lease
  acquisition, exclusion, completion, and release checks passed in every run.

## One-shard NDT-BPE strict smoke test

```text
commands=4103
errors=0
input_bytes=503327778
output_valid_bytes=519100232
tokens=129775058
nonzero_entry_samples=4103
runtime_workers=8
slots=8
max_inflight=8
local_wall_s=63.092
breakdown_cache_drop_us=18.166
fetch_s=4.426
sha256=147131246141dea04563cec2459b36e6584d7cab194630531af38663afb92095
```

The checksum is identical to the existing Ray-only and NDT-BPE reference
checksum for the same shard.  A separate root process observed `ETXTBSY` when
opening the real NVMe-oF output file for writing while the lease was active.
The same writable open succeeded immediately after COMPLETE, and the manifest
was present only after lease completion.  This run created and initialized a
new 2,012,487,680-byte output pool, so its wall time is a correctness smoke
result rather than a replacement performance measurement.

## Remaining boundary

The module now enforces a whole-file write/layout lease for host filesystem
access.  It intentionally does not reject ordinary reads or read-only mmap.
Correct consumers use the manifest as the publication boundary and therefore
do not consume an output while its lease is active.  Privileged raw
block-device access, another host, and storage software outside the NDT route
remain outside this Compute-kernel lease boundary.

SPDK currently completes each custom command after all extent writes complete,
but the custom write-back path does not issue a separate NVMe Flush.  The test
therefore verifies visibility and checksum correctness, not persistence across
power loss.
