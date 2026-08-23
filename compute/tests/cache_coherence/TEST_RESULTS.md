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
PASS: exact inode/file-offset invalidation exposed raw pattern B
PASS: BEGIN removed dirty host state before raw pattern D
```

This establishes both hazards and the intended behavior:

- COMPLETE removes a clean but stale buffered page after an out-of-band write.
- BEGIN writes back and removes dirty host state before storage obtains the LBA
  route, so that state cannot later overwrite the storage-side result.

## One-shard NDT-BPE strict smoke test

```text
commands=4103
errors=0
input_bytes=503327778
output_valid_bytes=519100232
tokens=129775058
nonzero_entry_samples=4103
breakdown_cache_drop_us=59.887
sha256=147131246141dea04563cec2459b36e6584d7cab194630531af38663afb92095
```

The checksum is identical to the existing Ray-only and NDT-BPE reference
checksum for the same shard.

## Remaining boundary

The module performs exact kernel page-cache range invalidation, but it does not
yet reject unrelated host reads, writes, truncation, hole punching, or writable
mmap while the range is lent to storage.  The integration is correct under the
current single-owner execution rule.  Claiming coherence under arbitrary
concurrent host access requires an enforced lease or a dedicated unmounted raw
output namespace.

SPDK currently completes each custom command after all extent writes complete,
but the custom write-back path does not issue a separate NVMe Flush.  The test
therefore verifies visibility and checksum correctness, not persistence across
power loss.
