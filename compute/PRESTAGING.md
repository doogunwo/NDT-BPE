# Persistent Pre-Staging

NDT-BPE now uses a metadata-only index over the original Arrow text buffers.
The index stores record lengths, bounded command groups, and physical NVMe
extents. It does not copy the text payload. SPDK loads the small index into a
DRAM metadata cache, gathers the original text bytes directly from their
extents, and constructs the existing `NKP1` input in memory before dispatching
it to the BPE runtime. The runtime therefore remains independent of Arrow.

## Commands

```bash
cd compute
source venv/bin/activate

# One-time ingestion. Reuses an already valid stage unless --force is supplied.
python ndt_stage.py prepare /mnt/nvme/openwebtext_disk/data-00000-of-00080.arrow \
  --stage-path /mnt/nvme/ndt-index/data-00000.ndtidx

# Validate source identity, format version, entry bounds, and payload layout.
python ndt_stage.py inspect /mnt/nvme/openwebtext_disk/data-00000-of-00080.arrow \
  --stage-path /mnt/nvme/ndt-index/data-00000.ndtidx

# Measured execution: never hide ingestion in the timed path.
sudo venv/bin/python ndt_stage.py run \
  /mnt/nvme/openwebtext_disk/data-00000-of-00080.arrow \
  --stage-path /mnt/nvme/ndt-index/data-00000.ndtidx \
  --stage-mode require-prestaged \
  --device /dev/ng2n1 \
  --output /mnt/nvme/results/data-00000.bin
```

Stage policies:

- `auto`: reuse a valid stage, otherwise build it once.
- `require-prestaged`: require a valid stage and fail without creating one.
- `rebuild`: atomically replace the stage even when the existing stage is valid.

## Validity and publication

The manifest records the source inode, size, and nanosecond mtime together with
the stage-format version. A mismatch makes the stage stale. Preparation uses an
exclusive lock, writes to a temporary file, synchronizes it, and publishes it
with an atomic rename. Concurrent consumers therefore see either the previous
valid stage or the completed replacement, never a partially written index.

The file contains only a compact header, command-entry table, source-extent
table, and record-length table. On the measured 503 MB shard it occupies
602,112 bytes and is loaded into SPDK DRAM once per target lifetime. Repeated
preload commands become cache lookups keyed by index LBA and length.
