#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector, std::string 자동 변환
#include <pybind11/functional.h>

#include "arrow_text_dump.h"
#include "extent-index.h"
#include "fallocate.h"
#include "io-uring.h"       // Ring, submit_nvme_passthru
#include "fiemap_schedule.h" // convert_fiemap_to_nvme_segs

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <sstream>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace py = pybind11;

namespace {

constexpr std::uint8_t kDefaultOpcode = 0xD4;
constexpr std::uint32_t kDefaultNsid = 1;
constexpr unsigned kDefaultQueueDepth = 256;
constexpr std::uint32_t kArrowChunkMagic = 0x41525458U; // "ARTX"
constexpr std::uint32_t kNdtPackedPayloadMagic = 0x31504b4eU; // "NKP1"
constexpr std::uint32_t kNdtPackedPayloadVersion = 1U;
constexpr std::size_t kNdtPackedPayloadHeaderBytes = 16U;
constexpr std::size_t kArrowChunkBytes = 128ULL * 1024ULL;
constexpr std::size_t kArrowChunkStrideBytes = kArrowChunkBytes;
constexpr std::size_t kArrowOutputPageBytes = 512ULL * 1024ULL;
constexpr std::size_t kArrowMaxFramedPayloadBytes = 96ULL * 1024ULL;
constexpr std::size_t kArrowMaxRecordFragmentBytes = 30ULL * 1024ULL;
constexpr std::uint32_t kBpeExtentDescriptorMagic = 0x45505842U; // 'BPXE'
constexpr std::uint16_t kBpeExtentDescriptorVersion = 1;
constexpr std::size_t kBpeExtentDescriptorBytes = 4096ULL;
constexpr std::uint32_t kBpeExtentDescriptorCdw12Flag = 0x80000000U;
constexpr std::uint32_t kBpeMetadataIndexCdw12Flag = 0x40000000U;
constexpr std::uint32_t kBpeMetadataIndexPreload = 0xFFFFFFFDU;
constexpr std::uint32_t kBpeMetadataIndexExecute = 0xFFFFFFFEU;
constexpr std::uint32_t kNdtMetadataIndexMagic = 0x4954444eU; // "NDTI"
constexpr std::uint32_t kNdtMetadataIndexVersion = 1U;
constexpr std::uint32_t kNdtOutputRoutesMagic = 0x5254554fU; // "OUTR"

struct __attribute__((packed)) ArrowChunkHeader {
    std::uint32_t magic = kArrowChunkMagic;
    std::uint16_t version = 2;
    std::uint16_t reserved = 0;
    std::uint32_t payload_offset = 0;
    std::uint32_t payload_length = 0;
    std::uint64_t data_range_offset = 0;
    std::int64_t batch_index = -1;
    std::int64_t num_rows = -1;
    std::uint32_t record_count = 0;
    std::uint32_t reserved2 = 0;
};

double now_us() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) / 1e3;
}

struct IoJob {
    std::uint64_t in_slba;
    std::uint64_t out_slba;
    std::uint32_t in_nblocks;
    std::uint32_t out_nblocks;
    std::uint32_t payload_valid_bytes;
    std::size_t index;
    bool input_is_extent_descriptor = false;
    bool input_is_metadata_index = false;
    bool output_write_enabled = false;
};

struct PendingInfo {
    std::uint64_t slba;
    std::uint64_t out_slba;
    std::uint32_t nblocks;
    std::size_t job_index;
    void* buf;
    std::size_t buf_len;
    std::uint32_t slot;
};

struct ScopedUnlink {
    std::string path;
    ~ScopedUnlink() {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
};

constexpr std::uint32_t kArrowMetaCacheMagic = 0x434d4145U; // "EAMC"
constexpr std::uint32_t kArrowMetaCacheVersion = 1;
constexpr std::uint32_t kArrowStageManifestMagic = 0x444d5341U; // "ASMD"
constexpr std::uint32_t kArrowStageManifestVersion = 16;

enum class ArrowStageMode {
    Auto,
    RequirePreStaged,
    Rebuild,
};

ArrowStageMode parse_arrow_stage_mode(const std::string& mode) {
    if (mode == "auto") {
        return ArrowStageMode::Auto;
    }
    if (mode == "require" || mode == "require-prestaged") {
        return ArrowStageMode::RequirePreStaged;
    }
    if (mode == "rebuild") {
        return ArrowStageMode::Rebuild;
    }
    throw std::invalid_argument(
        "stage_mode must be one of: auto, require-prestaged, rebuild");
}

struct ScopedStageLock {
    int fd = -1;

    explicit ScopedStageLock(const std::string& stage_path) {
        const std::string lock_path = stage_path + ".lock";
        fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            throw std::runtime_error(std::string("open stage lock failed: ") +
                                     std::strerror(errno));
        }
        if (::flock(fd, LOCK_EX) != 0) {
            const int saved = errno;
            ::close(fd);
            fd = -1;
            throw std::runtime_error(std::string("flock stage failed: ") +
                                     std::strerror(saved));
        }
    }

    ~ScopedStageLock() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }

    ScopedStageLock(const ScopedStageLock&) = delete;
    ScopedStageLock& operator=(const ScopedStageLock&) = delete;
};

struct ArrowMetaCacheHeader {
    std::uint32_t magic = kArrowMetaCacheMagic;
    std::uint32_t version = kArrowMetaCacheVersion;
    std::uint64_t inode = 0;
    std::uint64_t file_size = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::uint64_t payload_size = 0;
};

struct __attribute__((packed)) ArrowStageManifestHeader {
    std::uint32_t magic = kArrowStageManifestMagic;
    std::uint32_t version = kArrowStageManifestVersion;
    std::uint32_t header_size = 0;
    std::uint32_t entry_size = sizeof(std::uint64_t) * 4 + sizeof(std::int64_t) * 2 + sizeof(std::uint32_t) * 2;
    std::uint64_t inode = 0;
    std::uint64_t file_size = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::uint64_t chunk_count = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t payload_base_offset = 0;
};

struct __attribute__((packed)) ArrowStageManifestEntry {
    std::uint64_t slba = 0;              // direct staged payload SLBA
    std::uint64_t stage_offset = 0;      // staged data chunk offset
    std::uint64_t data_range_offset = 0;
    std::uint64_t payload_length = 0;
    std::int64_t batch_index = -1;
    std::int64_t num_rows = -1;
    std::uint32_t nblocks = 0;           // staged payload block count
    std::uint32_t reserved = 0;
};

struct __attribute__((packed)) BpeExtentDescriptorHeader {
    std::uint32_t magic = kBpeExtentDescriptorMagic;
    std::uint16_t version = kBpeExtentDescriptorVersion;
    std::uint16_t extent_count = 0;
    std::uint32_t total_blocks = 0;
    std::uint32_t reserved = 0;
};

struct __attribute__((packed)) BpeExtentDescriptorEntry {
    std::uint64_t slba = 0;
    std::uint32_t nblocks = 0;
    std::uint32_t reserved = 0;
};

struct __attribute__((packed)) NdtMetadataIndexHeader {
    std::uint32_t magic = kNdtMetadataIndexMagic;
    std::uint32_t version = kNdtMetadataIndexVersion;
    std::uint32_t header_size = 0;
    std::uint32_t entry_size = 0;
    std::uint32_t extent_size = 0;
    std::uint32_t reserved = 0;
    std::uint64_t inode = 0;
    std::uint64_t source_file_size = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::uint64_t index_bytes = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t extent_count = 0;
    std::uint64_t record_count = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t entries_offset = 0;
    std::uint64_t extents_offset = 0;
    std::uint64_t lengths_offset = 0;
};

struct __attribute__((packed)) NdtMetadataIndexEntry {
    std::uint32_t first_extent = 0;
    std::uint32_t extent_count = 0;
    std::uint32_t first_record = 0;
    std::uint32_t record_count = 0;
    std::uint32_t head_skip = 0;
    std::uint32_t raw_blocks = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t packed_bytes = 0;
};

struct __attribute__((packed)) NdtMetadataIndexExtent {
    std::uint64_t slba = 0;
    std::uint32_t nblocks = 0;
    std::uint32_t reserved = 0;
};

struct __attribute__((packed)) NdtOutputRoute {
    std::uint32_t first_extent = 0;
    std::uint32_t extent_count = 0;
    std::uint32_t capacity_bytes = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(NdtMetadataIndexHeader) == 120,
              "NdtMetadataIndexHeader layout changed");
static_assert(sizeof(NdtMetadataIndexEntry) == 32,
              "NdtMetadataIndexEntry layout changed");
static_assert(sizeof(NdtMetadataIndexExtent) == 16,
              "NdtMetadataIndexExtent layout changed");
static_assert(sizeof(NdtOutputRoute) == 16,
              "NdtOutputRoute layout changed");

static_assert(sizeof(ArrowStageManifestEntry) ==
                  sizeof(std::uint64_t) * 4 + sizeof(std::int64_t) * 2 + sizeof(std::uint32_t) * 2,
              "ArrowStageManifestEntry size changed");

std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

std::uint64_t div_round_up_u64(std::uint64_t value, std::uint64_t divisor) {
    return (value + divisor - 1) / divisor;
}

std::uint64_t arrow_stage_read_bytes(std::uint64_t payload_length) {
    return align_up_u64(payload_length, FIEMAP_LBA_BYTES);
}

void read_exact_at(int fd,
                   std::uint64_t offset,
                   void* buf,
                   std::size_t len,
                   const char* label);

void write_exact_at(int fd,
                    std::uint64_t offset,
                    const void* buf,
                    std::size_t len,
                    const char* label);

bool has_arrow_extension(const std::string& path) {
    const std::string suffix = ".arrow";
    if (path.size() < suffix.size()) {
        return false;
    }
    return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string make_arrow_meta_cache_path(const std::string& input_path) {
    return input_path + ".arrowmeta.dat";
}

std::string make_arrow_stage_metadata_path(const std::string& input_path,
                                           std::int64_t arrow_batch_start,
                                           std::int64_t arrow_batch_count) {
    if (arrow_batch_start < 0 && arrow_batch_count < 0) {
        return input_path + ".ndtidx";
    }
    return input_path + ".batch" + std::to_string(arrow_batch_start) +
           ".count" + std::to_string(arrow_batch_count) + ".ndtidx";
}

void fill_arrow_stage_source_identity(const std::string& input_path,
                                      ArrowStageManifestHeader& hdr) {
    struct stat st {};
    if (::stat(input_path.c_str(), &st) != 0) {
        throw std::runtime_error(std::string("stat input failed: ") + std::strerror(errno));
    }
    hdr.inode = static_cast<std::uint64_t>(st.st_ino);
    hdr.file_size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    hdr.mtime_sec = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    hdr.mtime_nsec = static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
    hdr.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
    hdr.mtime_nsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif
}

bool arrow_stage_source_matches(const std::string& input_path,
                                const ArrowStageManifestHeader& hdr) {
    ArrowStageManifestHeader expected{};
    fill_arrow_stage_source_identity(input_path, expected);
    return hdr.inode == expected.inode &&
           hdr.file_size == expected.file_size &&
           hdr.mtime_sec == expected.mtime_sec &&
           hdr.mtime_nsec == expected.mtime_nsec;
}

std::vector<NvmeSeg> map_file_range_to_nvme_segs(const std::string& filepath,
                                                 std::uint64_t file_offset,
                                                 std::uint64_t length,
                                                 std::size_t max_extents) {
    if (length == 0) {
        throw std::invalid_argument("file range length must be > 0");
    }
    const int fd = ::open(filepath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(std::string("open range fiemap failed: ") +
                                 std::strerror(errno));
    }

    const std::size_t extent_cap = std::max<std::size_t>(max_extents, 16);
    const std::size_t fm_bytes =
        sizeof(struct fiemap) + extent_cap * sizeof(struct fiemap_extent);
    std::vector<std::uint8_t> fm_storage(fm_bytes, 0);
    auto* fm = reinterpret_cast<struct fiemap*>(fm_storage.data());
    fm->fm_start = file_offset;
    fm->fm_length = length;
    fm->fm_flags = 0;
    fm->fm_extent_count = static_cast<std::uint32_t>(extent_cap);

    if (::ioctl(fd, FS_IOC_FIEMAP, fm) == -1) {
        const int saved = errno;
        ::close(fd);
        throw std::runtime_error(std::string("FS_IOC_FIEMAP(range) failed: ") +
                                 std::strerror(saved));
    }
    ::close(fd);

    const auto* extents = reinterpret_cast<const struct fiemap_extent*>(fm->fm_extents);
    const std::uint64_t end = file_offset + length;
    std::uint64_t covered_until = file_offset;
    std::vector<NvmeSeg> segs;

    for (std::uint32_t i = 0; i < fm->fm_mapped_extents && covered_until < end; ++i) {
        const auto& ext = extents[i];
        const std::uint64_t ext_start = ext.fe_logical;
        const std::uint64_t ext_end = ext.fe_logical + ext.fe_length;
        if (ext_end <= covered_until || ext_start >= end) {
            continue;
        }
        if (ext_start > covered_until) {
            throw std::runtime_error("file range has an unmapped hole at offset " +
                                     std::to_string(covered_until));
        }
        const std::uint64_t use_start = covered_until;
        const std::uint64_t use_end = std::min<std::uint64_t>(ext_end, end);
        const std::uint64_t physical = ext.fe_physical + (use_start - ext_start);
        const std::uint64_t bytes = use_end - use_start;
        if (physical % FIEMAP_LBA_BYTES != 0 || bytes % FIEMAP_LBA_BYTES != 0) {
            throw std::runtime_error("file range is not LBA aligned");
        }
        segs.push_back(NvmeSeg{physical / FIEMAP_LBA_BYTES,
                               static_cast<std::uint32_t>(bytes / FIEMAP_LBA_BYTES)});
        covered_until = use_end;
    }
    if (covered_until != end || segs.empty()) {
        throw std::runtime_error("file range is not fully mapped: offset=" +
                                 std::to_string(file_offset) + " length=" +
                                 std::to_string(length));
    }
    return segs;
}

NvmeSeg map_file_range_to_nvme_seg(const std::string& filepath,
                                   std::uint64_t file_offset,
                                   std::uint64_t length,
                                   std::size_t max_extents) {
    auto segs = map_file_range_to_nvme_segs(filepath, file_offset, length, max_extents);
    if (segs.empty()) {
        throw std::runtime_error("file range produced no segments");
    }
    std::uint64_t expected = segs.front().slba;
    std::uint32_t total = 0;
    for (const auto& seg : segs) {
        if (seg.slba != expected) {
            throw std::runtime_error("file range is not physically contiguous");
        }
        expected += seg.nblocks;
        total += seg.nblocks;
    }
    return NvmeSeg{segs.front().slba, total};
}

void write_bpe_extent_descriptor(int fd,
                                 std::uint64_t desc_offset,
                                 const std::vector<NvmeSeg>& data_segs,
                                 std::uint32_t total_blocks,
                                 std::uint32_t payload_valid_bytes) {
    constexpr std::size_t kMaxDescEntries =
        (kBpeExtentDescriptorBytes - sizeof(BpeExtentDescriptorHeader)) /
        sizeof(BpeExtentDescriptorEntry);
    if (data_segs.empty() || data_segs.size() > kMaxDescEntries) {
        throw std::runtime_error("too many extents for BPE descriptor: " +
                                 std::to_string(data_segs.size()));
    }
    std::vector<char> block(kBpeExtentDescriptorBytes, 0);
    BpeExtentDescriptorHeader hdr{};
    hdr.extent_count = static_cast<std::uint16_t>(data_segs.size());
    hdr.total_blocks = total_blocks;
    hdr.reserved = payload_valid_bytes;
    std::memcpy(block.data(), &hdr, sizeof(hdr));
    auto* entries = reinterpret_cast<BpeExtentDescriptorEntry*>(
        block.data() + sizeof(BpeExtentDescriptorHeader));
    for (std::size_t i = 0; i < data_segs.size(); ++i) {
        entries[i].slba = data_segs[i].slba;
        entries[i].nblocks = data_segs[i].nblocks;
    }
    write_exact_at(fd, desc_offset, block.data(), block.size(), "BPE extent descriptor");
}

bool try_load_arrow_stage_metadata(const std::string& input_path,
                                   const std::string& stage_path,
                                   std::vector<NvmeSeg>& in_segs,
                                   std::size_t& total_bytes,
                                   std::size_t max_extents,
                                   bool verbose) {
    (void)max_extents;
    struct stat stage_st {};
    if (::stat(stage_path.c_str(), &stage_st) != 0 ||
        stage_st.st_size < static_cast<off_t>(sizeof(ArrowStageManifestHeader))) {
        return false;
    }

    const int fd = ::open(stage_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    ArrowStageManifestHeader hdr{};
    try {
        read_exact_at(fd, 0, &hdr, sizeof(hdr), "arrow stage manifest");
    } catch (...) {
        ::close(fd);
        return false;
    }

    if (hdr.magic != kArrowStageManifestMagic ||
        hdr.version != kArrowStageManifestVersion ||
        hdr.header_size != sizeof(ArrowStageManifestHeader) ||
        hdr.entry_size != sizeof(ArrowStageManifestEntry) ||
        hdr.chunk_count == 0 ||
        !arrow_stage_source_matches(input_path, hdr)) {
        ::close(fd);
        return false;
    }

    const auto entry_bytes = static_cast<std::uint64_t>(hdr.chunk_count) *
                             sizeof(ArrowStageManifestEntry);
    if (entry_bytes / sizeof(ArrowStageManifestEntry) != hdr.chunk_count ||
        sizeof(ArrowStageManifestHeader) + entry_bytes >
            static_cast<std::uint64_t>(stage_st.st_size)) {
        ::close(fd);
        return false;
    }

    std::vector<ArrowStageManifestEntry> entries(
        static_cast<std::size_t>(hdr.chunk_count));
    try {
        read_exact_at(fd,
                      sizeof(ArrowStageManifestHeader),
                      entries.data(),
                      static_cast<std::size_t>(entry_bytes),
                      "arrow stage manifest entries");
    } catch (...) {
        ::close(fd);
        return false;
    }
    ::close(fd);

    in_segs.clear();
    in_segs.reserve(entries.size());
    total_bytes = 0;
    for (const auto& entry : entries) {
        const std::uint64_t read_bytes = arrow_stage_read_bytes(entry.payload_length);
        const std::uint32_t expected_blocks = static_cast<std::uint32_t>(read_bytes / FIEMAP_LBA_BYTES);
        if (entry.nblocks != expected_blocks ||
            entry.stage_offset < hdr.payload_base_offset ||
            (entry.stage_offset - hdr.payload_base_offset) % kArrowChunkStrideBytes != 0 ||
            read_bytes == 0 ||
            entry.stage_offset + read_bytes > static_cast<std::uint64_t>(stage_st.st_size) ||
            entry.payload_length > kArrowChunkBytes ||
            entry.payload_length == 0) {
            in_segs.clear();
            total_bytes = 0;
            return false;
        }
        in_segs.push_back(NvmeSeg{entry.slba, entry.nblocks, static_cast<std::uint32_t>(entry.payload_length)});
        total_bytes += static_cast<std::size_t>(read_bytes);
    }
    if (total_bytes != hdr.total_bytes) {
        in_segs.clear();
        total_bytes = 0;
        return false;
    }

    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] arrow metadata manifest cache hit: %s chunks=%zu bytes=%zu\n",
                     stage_path.c_str(),
                     in_segs.size(),
                     total_bytes);
    }
    return true;
}

ArrowMetaCacheHeader make_arrow_meta_cache_header(const std::string& input_path,
                                                  std::uint64_t payload_size) {
    struct stat st {};
    if (::stat(input_path.c_str(), &st) != 0) {
        throw std::runtime_error(std::string("stat input failed: ") + std::strerror(errno));
    }
    ArrowMetaCacheHeader hdr{};
    hdr.inode = static_cast<std::uint64_t>(st.st_ino);
    hdr.file_size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    hdr.mtime_sec = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    hdr.mtime_nsec = static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
    hdr.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
    hdr.mtime_nsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif
    hdr.payload_size = payload_size;
    return hdr;
}

bool try_load_cached_extent_index(const std::string& input_path,
                                  ExtentIndex* out_index,
                                  bool verbose) {
    const std::string cache_path = make_arrow_meta_cache_path(input_path);
    struct stat cache_st {};
    if (::stat(cache_path.c_str(), &cache_st) != 0) {
        return false;
    }

    ffilesystem cache_fs;
    cache_fs.set_metadata_file_path(cache_path);
    cache_fs.load_metadata();
    auto bytes = cache_fs.load_blob_copy();
    if (bytes.size() < sizeof(ArrowMetaCacheHeader)) {
        return false;
    }

    ArrowMetaCacheHeader hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));
    if (hdr.magic != kArrowMetaCacheMagic || hdr.version != kArrowMetaCacheVersion) {
        return false;
    }
    if (bytes.size() < sizeof(hdr) + hdr.payload_size) {
        return false;
    }

    const auto expected = make_arrow_meta_cache_header(input_path, hdr.payload_size);
    if (hdr.inode != expected.inode || hdr.file_size != expected.file_size ||
        hdr.mtime_sec != expected.mtime_sec || hdr.mtime_nsec != expected.mtime_nsec) {
        return false;
    }

    std::string error;
    if (!DeserializeExtentIndexBinary(bytes.data() + sizeof(hdr),
                                      static_cast<std::size_t>(hdr.payload_size),
                                      out_index,
                                      &error)) {
        if (verbose) {
            std::fprintf(stderr, "[INFO] arrow metadata cache decode miss: %s\n", error.c_str());
        }
        return false;
    }

    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] arrow metadata cache hit: %s buffers=%zu\n",
                     cache_path.c_str(),
                     out_index->buffers.size());
    }
    return true;
}

void save_cached_extent_index(const std::string& input_path,
                              const ExtentIndex& index,
                              bool verbose) {
    std::vector<std::uint8_t> payload;
    std::string error;
    if (!SerializeExtentIndexBinary(index, &payload, &error)) {
        throw std::runtime_error("extent-index serialize failed: " + error);
    }

    const auto hdr = make_arrow_meta_cache_header(input_path, payload.size());
    std::vector<std::uint8_t> blob(sizeof(hdr) + payload.size());
    std::memcpy(blob.data(), &hdr, sizeof(hdr));
    if (!payload.empty()) {
        std::memcpy(blob.data() + sizeof(hdr), payload.data(), payload.size());
    }

    const std::string cache_path = make_arrow_meta_cache_path(input_path);
    const std::string tmp_path =
        cache_path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));

    ffilesystem cache_fs;
    cache_fs.set_metadata_file_path(tmp_path);
    cache_fs.store_blob(blob.data(), blob.size(), true);

    if (::rename(tmp_path.c_str(), cache_path.c_str()) != 0) {
        const int saved_errno = errno;
        ::unlink(tmp_path.c_str());
        throw std::system_error(saved_errno,
                                std::generic_category(),
                                "arrow metadata cache rename failed");
    }

    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] arrow metadata cache saved: %s bytes=%zu buffers=%zu\n",
                     cache_path.c_str(),
                     blob.size(),
                     index.buffers.size());
    }
}

void read_exact_at(int fd,
                   std::uint64_t offset,
                   void* buf,
                   std::size_t len,
                   const char* label) {
    auto* dst = static_cast<std::uint8_t*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t rc = ::pread(fd,
                                   dst + done,
                                   len - done,
                                   static_cast<off_t>(offset + done));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string(label) + " pread failed: " +
                                     std::strerror(errno));
        }
        if (rc == 0) {
            throw std::runtime_error(std::string(label) + " pread hit EOF");
        }
        done += static_cast<std::size_t>(rc);
    }
}

void write_exact_at(int fd,
                    std::uint64_t offset,
                    const void* buf,
                    std::size_t len,
                    const char* label) {
    const auto* src = static_cast<const std::uint8_t*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t rc = ::pwrite(fd,
                                    src + done,
                                    len - done,
                                    static_cast<off_t>(offset + done));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string(label) + " pwrite failed: " +
                                     std::strerror(errno));
        }
        done += static_cast<std::size_t>(rc);
    }
}

std::size_t utf8_safe_prefix_len(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text.size();
    }
    std::size_t split = max_bytes;
    while (split > 0) {
        const unsigned char c = static_cast<unsigned char>(text[split]);
        if ((c & 0xC0U) != 0x80U) {
            break;
        }
        --split;
    }
    if (split == 0) {
        split = max_bytes;
    }
    return split;
}

template <typename Fn>
void for_each_text_fragment(const std::string& text,
                            std::size_t payload_limit,
                            Fn&& fn) {
    if (text.empty()) {
        return;
    }
    if (text.size() > payload_limit) {
        throw std::runtime_error("text record exceeds bounded payload size; larger input bank or record-level continuation is required");
    }
    fn(std::string_view(text.data(), text.size()));
}

void append_u32_le(std::vector<char>& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24) & 0xFFU));
}

struct PackedTextPayloadBuilder {
    explicit PackedTextPayloadBuilder(std::size_t payload_limit)
        : limit(payload_limit) {}

    std::size_t encoded_size_with(std::size_t next_len) const {
        return kNdtPackedPayloadHeaderBytes +
               (records.size() + 1U) * sizeof(std::uint32_t) +
               bytes + next_len;
    }

    std::size_t encoded_size() const {
        return kNdtPackedPayloadHeaderBytes +
               records.size() * sizeof(std::uint32_t) +
               bytes;
    }

    bool empty() const {
        return records.empty();
    }

    void clear() {
        first_batch_index = -1;
        records.clear();
        bytes = 0;
    }

    void add(std::int64_t batch_index, const std::string& text) {
        if (text.empty()) {
            return;
        }
        if (first_batch_index < 0) {
            first_batch_index = batch_index;
        }
        records.push_back(text);
        bytes += text.size();
    }

    std::vector<char> encode() const {
        if (records.empty()) {
            return {};
        }
        if (records.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("too many records in one NDT packed payload");
        }

        const std::uint32_t record_count =
            static_cast<std::uint32_t>(records.size());
        const std::uint32_t data_offset = static_cast<std::uint32_t>(
            kNdtPackedPayloadHeaderBytes +
            record_count * sizeof(std::uint32_t));

        std::vector<char> out;
        out.reserve(encoded_size());
        append_u32_le(out, kNdtPackedPayloadMagic);
        append_u32_le(out, kNdtPackedPayloadVersion);
        append_u32_le(out, record_count);
        append_u32_le(out, data_offset);

        for (const auto& rec : records) {
            if (rec.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("text record exceeds u32 packed length");
            }
            append_u32_le(out, static_cast<std::uint32_t>(rec.size()));
        }
        for (const auto& rec : records) {
            out.insert(out.end(), rec.begin(), rec.end());
        }

        if (out.size() > limit) {
            throw std::runtime_error("NDT packed payload exceeds payload limit");
        }
        return out;
    }

    std::size_t limit = 0;
    std::int64_t first_batch_index = -1;
    std::vector<std::string> records;
    std::size_t bytes = 0;
};


template <typename Fn>
void for_each_arrow_text_record(const std::string& input_path,
                                std::int64_t effective_start,
                                std::int64_t effective_end,
                                Fn&& fn) {
    auto mmap_result = arrow::io::MemoryMappedFile::Open(input_path, arrow::io::FileMode::READ);
    if (!mmap_result.ok()) {
        throw std::runtime_error("open Arrow mmap failed: " + mmap_result.status().ToString());
    }
    auto mmap = mmap_result.ValueOrDie();
    auto file_result = arrow::ipc::RecordBatchFileReader::Open(mmap);
    auto process_batch = [&](const std::shared_ptr<arrow::RecordBatch>& batch,
                             std::int64_t batch_index) {
        if (batch_index < effective_start || batch_index >= effective_end) {
            return;
        }
        const int column_index = batch->schema()->GetFieldIndex("text");
        if (column_index < 0) {
            throw std::runtime_error("Arrow column 'text' not found");
        }
        auto array = batch->column(column_index);
        if (array->type_id() == arrow::Type::STRING) {
            auto strings = std::static_pointer_cast<arrow::StringArray>(array);
            for (std::int64_t row = 0; row < strings->length(); ++row) {
                if (!strings->IsNull(row)) {
                    fn(static_cast<std::int64_t>(batch_index), strings->GetString(row));
                }
            }
        } else if (array->type_id() == arrow::Type::LARGE_STRING) {
            auto strings = std::static_pointer_cast<arrow::LargeStringArray>(array);
            for (std::int64_t row = 0; row < strings->length(); ++row) {
                if (!strings->IsNull(row)) {
                    fn(static_cast<std::int64_t>(batch_index), strings->GetString(row));
                }
            }
        } else {
            throw std::runtime_error("Arrow column 'text' is not string/large_string");
        }
    };

    if (file_result.ok()) {
        auto reader = file_result.ValueOrDie();
        for (int batch_index = 0; batch_index < reader->num_record_batches(); ++batch_index) {
            auto batch_result = reader->ReadRecordBatch(batch_index);
            if (!batch_result.ok()) {
                throw std::runtime_error("read Arrow record batch failed: " +
                                         batch_result.status().ToString());
            }
            process_batch(batch_result.ValueOrDie(), batch_index);
        }
        return;
    }

    auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(mmap);
    if (!stream_result.ok()) {
        throw std::runtime_error("open Arrow stream reader failed: " +
                                 stream_result.status().ToString());
    }
    auto reader = stream_result.ValueOrDie();
    std::int64_t batch_index = 0;
    for (;;) {
        auto batch_result = reader->ReadNext();
        if (!batch_result.ok()) {
            throw std::runtime_error("read Arrow stream batch failed: " +
                                     batch_result.status().ToString());
        }
        auto batch_with_meta = batch_result.ValueOrDie();
        if (!batch_with_meta.batch) {
            break;
        }
        process_batch(batch_with_meta.batch, batch_index++);
    }
}

void build_arrow_stage_file(const std::string& input_path,
                            const std::string& stage_path,
                            std::vector<NvmeSeg>& in_segs,
                            std::size_t& total_bytes,
                            std::size_t max_extents,
                            std::int64_t arrow_batch_start,
                            std::int64_t arrow_batch_count,
                            ArrowStageMode stage_mode,
                            bool* cache_hit,
                            bool verbose) {
    if (cache_hit) {
        *cache_hit = false;
    }

    ScopedStageLock stage_lock(stage_path);
    if (stage_mode != ArrowStageMode::Rebuild) {
        if (try_load_arrow_stage_metadata(input_path,
                                          stage_path,
                                          in_segs,
                                          total_bytes,
                                          max_extents,
                                          verbose)) {
            if (cache_hit) {
                *cache_hit = true;
            }
            return;
        }
        if (stage_mode == ArrowStageMode::RequirePreStaged) {
            throw std::runtime_error(
                "valid pre-staged NDT index is required but missing or stale: " +
                stage_path);
        }
    }

    constexpr std::size_t kArrowChunkPayloadBytes = kArrowChunkBytes;
    const std::int64_t effective_start =
        (arrow_batch_start < 0) ? 0 : arrow_batch_start;
    const std::int64_t effective_end =
        (arrow_batch_count < 0)
            ? std::numeric_limits<std::int64_t>::max()
            : effective_start + arrow_batch_count;

    std::size_t planned_chunks = 0;
    PackedTextPayloadBuilder planning_pack(kArrowChunkPayloadBytes);
    auto flush_planning_pack = [&]() {
        if (!planning_pack.empty()) {
            ++planned_chunks;
            planning_pack.clear();
        }
    };
    for_each_arrow_text_record(
        input_path, effective_start, effective_end,
        [&](std::int64_t batch_index, const std::string& text) {
            if (text.empty()) {
                return;
            }
            if (kNdtPackedPayloadHeaderBytes + sizeof(std::uint32_t) +
                    text.size() >
                kArrowChunkPayloadBytes) {
                throw std::runtime_error("text record exceeds bounded packed payload size; larger input bank or record-level continuation is required");
            }
            if (!planning_pack.empty() &&
                planning_pack.encoded_size_with(text.size()) >
                    kArrowChunkPayloadBytes) {
                flush_planning_pack();
            }
            planning_pack.add(batch_index, text);
        });
    flush_planning_pack();
    if (planned_chunks == 0) {
        throw std::runtime_error("no arrow text buffers selected for staging");
    }

    const std::uint64_t manifest_bytes =
        sizeof(ArrowStageManifestHeader) +
        static_cast<std::uint64_t>(planned_chunks) * sizeof(ArrowStageManifestEntry);
    const std::uint64_t payload_base_offset =
        align_up_u64(manifest_bytes, kArrowChunkStrideBytes);

    const std::string temp_stage_path =
        stage_path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    ScopedUnlink temp_stage_cleanup{temp_stage_path};
    const int stage_fd = ::open(temp_stage_path.c_str(),
                                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
                                0644);
    if (stage_fd < 0) {
        throw std::runtime_error(std::string("open stage failed: ") + std::strerror(errno));
    }

    const std::uint64_t stage_file_size =
        payload_base_offset + static_cast<std::uint64_t>(planned_chunks) * kArrowChunkStrideBytes;
    if (::ftruncate(stage_fd, static_cast<off_t>(stage_file_size)) != 0) {
        const int saved = errno;
        ::close(stage_fd);
        throw std::runtime_error(std::string("ftruncate(stage) failed: ") +
                                 std::strerror(saved));
    }
    const long falloc_rc = ::syscall(SYS_fallocate,
                                    stage_fd,
                                    0,
                                    0,
                                    static_cast<off_t>(stage_file_size));
    if (falloc_rc != 0 && errno != EOPNOTSUPP && errno != ENOSYS && errno != EINVAL) {
        const int saved = errno;
        ::close(stage_fd);
        throw std::runtime_error(std::string("fallocate(stage) failed: ") +
                                 std::strerror(saved));
    }

    std::size_t chunk_count = 0;
    std::vector<ArrowStageManifestEntry> entries;
    entries.reserve(planned_chunks);
    std::vector<char> chunk(kArrowChunkBytes, 0);

    try {
        auto emit_payload = [&](std::int64_t batch_index,
                                std::int64_t num_rows,
                                const std::vector<char>& payload) {
            if (payload.empty()) {
                return;
            }
            if (payload.size() > kArrowChunkPayloadBytes) {
                throw std::runtime_error("NDT packed payload exceeds chunk payload limit");
            }
            std::fill(chunk.begin(), chunk.end(), 0);
            std::memcpy(chunk.data(), payload.data(), payload.size());

            const std::uint64_t stage_off =
                payload_base_offset +
                static_cast<std::uint64_t>(chunk_count) * kArrowChunkStrideBytes;
            write_exact_at(stage_fd, stage_off, chunk.data(), chunk.size(), "NDT packed text stage");

            ArrowStageManifestEntry entry{};
            entry.stage_offset = stage_off;
            entry.payload_length = payload.size();
            entry.batch_index = batch_index;
            entry.num_rows = num_rows;
            entries.push_back(entry);
            ++chunk_count;
        };

        PackedTextPayloadBuilder emit_pack(kArrowChunkPayloadBytes);
        auto flush_emit_pack = [&]() {
            if (emit_pack.empty()) {
                return;
            }
            const std::int64_t batch_index = emit_pack.first_batch_index;
            const std::int64_t num_rows =
                static_cast<std::int64_t>(emit_pack.records.size());
            const std::vector<char> payload = emit_pack.encode();
            emit_payload(batch_index, num_rows, payload);
            emit_pack.clear();
        };

        for_each_arrow_text_record(
            input_path, effective_start, effective_end,
            [&](std::int64_t batch_index, const std::string& text) {
                if (text.empty()) {
                    return;
                }
                if (kNdtPackedPayloadHeaderBytes + sizeof(std::uint32_t) +
                        text.size() >
                    kArrowChunkPayloadBytes) {
                    throw std::runtime_error("text record exceeds bounded packed payload size; larger input bank or record-level continuation is required");
                }
                if (!emit_pack.empty() &&
                    emit_pack.encoded_size_with(text.size()) >
                        kArrowChunkPayloadBytes) {
                    flush_emit_pack();
                }
                emit_pack.add(batch_index, text);
            });
        flush_emit_pack();

        if (chunk_count == 0 || entries.size() != planned_chunks) {
            throw std::runtime_error("no arrow text buffers selected for staging");
        }

        if (::fdatasync(stage_fd) != 0) {
            throw std::runtime_error(std::string("fdatasync(stage) failed: ") +
                                     std::strerror(errno));
        }

        in_segs.clear();
        in_segs.reserve(entries.size());
        total_bytes = 0;
        for (auto& entry : entries) {
            const std::uint64_t read_bytes = arrow_stage_read_bytes(entry.payload_length);
            if (read_bytes == 0 || read_bytes > kArrowChunkBytes) {
                throw std::runtime_error("arrow stage read size is invalid");
            }
            const auto data_segs =
                map_file_range_to_nvme_segs(temp_stage_path,
                                            entry.stage_offset,
                                            read_bytes,
                                            max_extents);
            const std::uint32_t data_blocks =
                static_cast<std::uint32_t>(read_bytes / FIEMAP_LBA_BYTES);
            if (data_segs.size() != 1 || data_segs[0].nblocks != data_blocks) {
                throw std::runtime_error("staged text payload is not physically contiguous; SPDK expects the command read LBA to point directly to payload");
            }
            entry.slba = data_segs[0].slba;
            entry.nblocks = data_blocks;
            in_segs.push_back(NvmeSeg{entry.slba, entry.nblocks, static_cast<std::uint32_t>(entry.payload_length)});
            total_bytes += static_cast<std::size_t>(read_bytes);
        }

        ArrowStageManifestHeader manifest{};
        manifest.header_size = sizeof(ArrowStageManifestHeader);
        manifest.entry_size = sizeof(ArrowStageManifestEntry);
        fill_arrow_stage_source_identity(input_path, manifest);
        manifest.chunk_count = entries.size();
        manifest.total_bytes = total_bytes;
        manifest.payload_base_offset = payload_base_offset;
        write_exact_at(stage_fd,
                       0,
                       &manifest,
                       sizeof(manifest),
                       "arrow stage manifest");
        write_exact_at(stage_fd,
                       sizeof(manifest),
                       entries.data(),
                       entries.size() * sizeof(ArrowStageManifestEntry),
                       "arrow stage manifest entries");
        if (::fdatasync(stage_fd) != 0) {
            throw std::runtime_error(std::string("fdatasync(manifest) failed: ") +
                                     std::strerror(errno));
        }
    } catch (...) {
        ::close(stage_fd);
        throw;
    }

    ::close(stage_fd);

    if (::rename(temp_stage_path.c_str(), stage_path.c_str()) != 0) {
        throw std::runtime_error(std::string("publish pre-staged index failed: ") +
                                 std::strerror(errno));
    }
    temp_stage_cleanup.path.clear();

    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] extent-index buffers=%zu staged_chunks=%zu segments=%zu metadata=%s manifest_bytes=%llu payload_base=%llu\n",
                     entries.size(),
                     chunk_count,
                     in_segs.size(),
                     stage_path.c_str(),
                     static_cast<unsigned long long>(manifest_bytes),
                     static_cast<unsigned long long>(payload_base_offset));
    }
}

void fill_ndt_index_source_identity(const std::string& input_path,
                                    NdtMetadataIndexHeader& hdr) {
    struct stat st {};
    if (::stat(input_path.c_str(), &st) != 0) {
        throw std::runtime_error(std::string("stat input failed: ") +
                                 std::strerror(errno));
    }
    hdr.inode = static_cast<std::uint64_t>(st.st_ino);
    hdr.source_file_size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    hdr.mtime_sec = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    hdr.mtime_nsec = static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
    hdr.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
    hdr.mtime_nsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif
}

bool ndt_index_source_matches(const std::string& input_path,
                              const NdtMetadataIndexHeader& hdr) {
    NdtMetadataIndexHeader expected{};
    fill_ndt_index_source_identity(input_path, expected);
    return hdr.inode == expected.inode &&
           hdr.source_file_size == expected.source_file_size &&
           hdr.mtime_sec == expected.mtime_sec &&
           hdr.mtime_nsec == expected.mtime_nsec;
}

std::uint64_t mapped_file_offset(const void* address, std::size_t length) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + length;
    if (end < begin) {
        throw std::runtime_error("mapped Arrow buffer address overflow");
    }
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream input(line);
        std::string address_range;
        std::string perms;
        std::string file_offset_hex;
        if (!(input >> address_range >> perms >> file_offset_hex)) {
            continue;
        }
        const auto dash = address_range.find('-');
        if (dash == std::string::npos) {
            continue;
        }
        const auto map_begin = static_cast<std::uintptr_t>(
            std::stoull(address_range.substr(0, dash), nullptr, 16));
        const auto map_end = static_cast<std::uintptr_t>(
            std::stoull(address_range.substr(dash + 1), nullptr, 16));
        if (map_begin <= begin && end <= map_end) {
            const auto map_file_offset = std::stoull(file_offset_hex, nullptr, 16);
            return map_file_offset + static_cast<std::uint64_t>(begin - map_begin);
        }
    }
    throw std::runtime_error(
        "Arrow value buffer is not backed by a directly addressable file mapping");
}

bool try_load_arrow_metadata_index(const std::string& input_path,
                                   const std::string& index_path,
                                   std::vector<NvmeSeg>& in_segs,
                                   std::size_t& total_bytes,
                                   std::size_t max_extents,
                                   bool verbose) {
    struct stat st {};
    if (::stat(index_path.c_str(), &st) != 0 ||
        st.st_size < static_cast<off_t>(sizeof(NdtMetadataIndexHeader))) {
        return false;
    }
    const int fd = ::open(index_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    NdtMetadataIndexHeader hdr{};
    try {
        read_exact_at(fd, 0, &hdr, sizeof(hdr), "NDT metadata index header");
    } catch (...) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    const std::uint64_t entry_bytes =
        hdr.entry_count * sizeof(NdtMetadataIndexEntry);
    const std::uint64_t extent_bytes =
        hdr.extent_count * sizeof(NdtMetadataIndexExtent);
    const std::uint64_t length_bytes = hdr.record_count * sizeof(std::uint32_t);
    if (hdr.magic != kNdtMetadataIndexMagic ||
        hdr.version != kNdtMetadataIndexVersion ||
        hdr.header_size != sizeof(NdtMetadataIndexHeader) ||
        hdr.entry_size != sizeof(NdtMetadataIndexEntry) ||
        hdr.extent_size != sizeof(NdtMetadataIndexExtent) ||
        hdr.entry_count == 0 || hdr.index_bytes == 0 ||
        hdr.index_bytes != static_cast<std::uint64_t>(st.st_size) ||
        hdr.entries_offset != sizeof(NdtMetadataIndexHeader) ||
        hdr.extents_offset != hdr.entries_offset + entry_bytes ||
        hdr.lengths_offset != hdr.extents_offset + extent_bytes ||
        hdr.lengths_offset + length_bytes > hdr.index_bytes ||
        !ndt_index_source_matches(input_path, hdr)) {
        return false;
    }
    NvmeSeg index_seg{};
    try {
        index_seg = map_file_range_to_nvme_seg(index_path,
                                               0,
                                               hdr.index_bytes,
                                               max_extents);
    } catch (...) {
        return false;
    }
    in_segs.clear();
    in_segs.reserve(static_cast<std::size_t>(hdr.entry_count));
    for (std::uint64_t i = 0; i < hdr.entry_count; ++i) {
        in_segs.push_back(NvmeSeg{index_seg.slba,
                                  index_seg.nblocks,
                                  static_cast<std::uint32_t>(i)});
    }
    total_bytes = static_cast<std::size_t>(hdr.payload_bytes);
    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] NDT metadata index cache hit: %s entries=%llu bytes=%llu\n",
                     index_path.c_str(),
                     static_cast<unsigned long long>(hdr.entry_count),
                     static_cast<unsigned long long>(hdr.index_bytes));
    }
    return true;
}

void build_arrow_metadata_index(const std::string& input_path,
                                const std::string& index_path,
                                std::vector<NvmeSeg>& in_segs,
                                std::size_t& total_bytes,
                                std::size_t max_extents,
                                std::int64_t arrow_batch_start,
                                std::int64_t arrow_batch_count,
                                ArrowStageMode stage_mode,
                                bool* cache_hit,
                                bool verbose) {
    if (cache_hit) {
        *cache_hit = false;
    }
    ScopedStageLock stage_lock(index_path);
    if (stage_mode != ArrowStageMode::Rebuild &&
        try_load_arrow_metadata_index(input_path, index_path, in_segs,
                                      total_bytes, max_extents, verbose)) {
        if (cache_hit) {
            *cache_hit = true;
        }
        return;
    }
    if (stage_mode == ArrowStageMode::RequirePreStaged) {
        throw std::runtime_error(
            "valid pre-staged NDT metadata index is required but missing or stale: " +
            index_path);
    }

    const std::int64_t effective_start = arrow_batch_start < 0 ? 0 : arrow_batch_start;
    const std::int64_t effective_end = arrow_batch_count < 0
        ? std::numeric_limits<std::int64_t>::max()
        : effective_start + arrow_batch_count;
    std::vector<NdtMetadataIndexEntry> entries;
    std::vector<NdtMetadataIndexExtent> extents;
    std::vector<std::uint32_t> lengths;

    auto mmap_result = arrow::io::MemoryMappedFile::Open(input_path,
                                                          arrow::io::FileMode::READ);
    if (!mmap_result.ok()) {
        throw std::runtime_error("open Arrow mmap failed: " +
                                 mmap_result.status().ToString());
    }
    auto mmap = mmap_result.ValueOrDie();

    auto process_batch = [&](const std::shared_ptr<arrow::RecordBatch>& batch,
                             std::int64_t batch_index) {
        if (batch_index < effective_start || batch_index >= effective_end) {
            return;
        }
        const int column_index = batch->schema()->GetFieldIndex("text");
        if (column_index < 0) {
            throw std::runtime_error("Arrow column 'text' not found");
        }
        auto array = batch->column(column_index);
        if (array->type_id() != arrow::Type::STRING) {
            throw std::runtime_error(
                "metadata-only index currently requires Arrow string text column");
        }
        auto strings = std::static_pointer_cast<arrow::StringArray>(array);
        auto value_data = strings->value_data();
        if (!value_data || value_data->size() == 0) {
            return;
        }
        const std::uint64_t value_file_offset =
            mapped_file_offset(value_data->data(), value_data->size());

        std::vector<std::uint32_t> group_lengths;
        std::uint64_t group_start = 0;
        std::uint64_t group_end = 0;
        std::size_t group_payload_bytes = 0;
        auto flush_group = [&]() {
            if (group_lengths.empty()) {
                return;
            }
            const std::uint64_t logical_start = value_file_offset + group_start;
            const std::uint64_t logical_end = value_file_offset + group_end;
            const std::uint64_t aligned_start =
                (logical_start / FIEMAP_LBA_BYTES) * FIEMAP_LBA_BYTES;
            const std::uint64_t aligned_end =
                align_up_u64(logical_end, FIEMAP_LBA_BYTES);
            const auto source_segs = map_file_range_to_nvme_segs(
                input_path, aligned_start, aligned_end - aligned_start, max_extents);
            NdtMetadataIndexEntry entry{};
            entry.first_extent = static_cast<std::uint32_t>(extents.size());
            entry.extent_count = static_cast<std::uint32_t>(source_segs.size());
            entry.first_record = static_cast<std::uint32_t>(lengths.size());
            entry.record_count = static_cast<std::uint32_t>(group_lengths.size());
            entry.head_skip = static_cast<std::uint32_t>(logical_start - aligned_start);
            entry.raw_blocks = static_cast<std::uint32_t>(
                (aligned_end - aligned_start) / FIEMAP_LBA_BYTES);
            entry.payload_bytes = static_cast<std::uint32_t>(group_payload_bytes);
            entry.packed_bytes = static_cast<std::uint32_t>(
                kNdtPackedPayloadHeaderBytes +
                group_lengths.size() * sizeof(std::uint32_t) +
                group_payload_bytes);
            for (const auto& seg : source_segs) {
                extents.push_back(NdtMetadataIndexExtent{seg.slba, seg.nblocks, 0});
            }
            lengths.insert(lengths.end(), group_lengths.begin(), group_lengths.end());
            entries.push_back(entry);
            group_lengths.clear();
            group_payload_bytes = 0;
        };

        for (std::int64_t row = 0; row < strings->length(); ++row) {
            if (strings->IsNull(row)) {
                flush_group();
                continue;
            }
            const auto begin = static_cast<std::uint64_t>(strings->value_offset(row));
            const auto len = static_cast<std::uint64_t>(strings->value_length(row));
            if (len == 0) {
                continue;
            }
            if (kNdtPackedPayloadHeaderBytes + sizeof(std::uint32_t) + len >
                kArrowChunkBytes) {
                throw std::runtime_error(
                    "text record exceeds metadata-index bounded payload size");
            }
            const std::size_t next_packed = kNdtPackedPayloadHeaderBytes +
                (group_lengths.size() + 1U) * sizeof(std::uint32_t) +
                group_payload_bytes + static_cast<std::size_t>(len);
            if (!group_lengths.empty() && next_packed > kArrowChunkBytes) {
                flush_group();
            }
            if (group_lengths.empty()) {
                group_start = begin;
            } else if (begin != group_end) {
                throw std::runtime_error("Arrow text values are not contiguous");
            }
            group_end = begin + len;
            group_payload_bytes += static_cast<std::size_t>(len);
            group_lengths.push_back(static_cast<std::uint32_t>(len));
        }
        flush_group();
    };

    auto file_result = arrow::ipc::RecordBatchFileReader::Open(mmap);
    if (file_result.ok()) {
        auto reader = file_result.ValueOrDie();
        for (int i = 0; i < reader->num_record_batches(); ++i) {
            auto batch_result = reader->ReadRecordBatch(i);
            if (!batch_result.ok()) {
                throw std::runtime_error("read Arrow record batch failed: " +
                                         batch_result.status().ToString());
            }
            process_batch(batch_result.ValueOrDie(), i);
        }
    } else {
        auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(mmap);
        if (!stream_result.ok()) {
            throw std::runtime_error("open Arrow stream reader failed: " +
                                     stream_result.status().ToString());
        }
        auto reader = stream_result.ValueOrDie();
        std::int64_t batch_index = 0;
        for (;;) {
            auto batch_result = reader->ReadNext();
            if (!batch_result.ok()) {
                throw std::runtime_error("read Arrow stream batch failed: " +
                                         batch_result.status().ToString());
            }
            auto with_meta = batch_result.ValueOrDie();
            if (!with_meta.batch) {
                break;
            }
            process_batch(with_meta.batch, batch_index++);
        }
    }
    if (entries.empty() || lengths.empty() || extents.empty()) {
        throw std::runtime_error("no Arrow text metadata selected for indexing");
    }

    NdtMetadataIndexHeader hdr{};
    hdr.header_size = sizeof(hdr);
    hdr.entry_size = sizeof(NdtMetadataIndexEntry);
    hdr.extent_size = sizeof(NdtMetadataIndexExtent);
    fill_ndt_index_source_identity(input_path, hdr);
    hdr.entry_count = entries.size();
    hdr.extent_count = extents.size();
    hdr.record_count = lengths.size();
    for (const auto& entry : entries) {
        hdr.payload_bytes += entry.packed_bytes;
    }
    hdr.entries_offset = sizeof(hdr);
    hdr.extents_offset = hdr.entries_offset +
        entries.size() * sizeof(NdtMetadataIndexEntry);
    hdr.lengths_offset = hdr.extents_offset +
        extents.size() * sizeof(NdtMetadataIndexExtent);
    hdr.index_bytes = align_up_u64(
        hdr.lengths_offset + lengths.size() * sizeof(std::uint32_t),
        FIEMAP_LBA_BYTES);

    const std::string temp_path =
        index_path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    ScopedUnlink cleanup{temp_path};
    const int fd = ::open(temp_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error(std::string("open metadata index failed: ") +
                                 std::strerror(errno));
    }
    try {
        if (::ftruncate(fd, static_cast<off_t>(hdr.index_bytes)) != 0) {
            throw std::runtime_error(std::string("ftruncate metadata index failed: ") +
                                     std::strerror(errno));
        }
        const long falloc_rc = ::syscall(SYS_fallocate, fd, 0, 0,
                                         static_cast<off_t>(hdr.index_bytes));
        if (falloc_rc != 0 && errno != EOPNOTSUPP && errno != ENOSYS && errno != EINVAL) {
            throw std::runtime_error(std::string("fallocate metadata index failed: ") +
                                     std::strerror(errno));
        }
        write_exact_at(fd, 0, &hdr, sizeof(hdr), "NDT metadata index header");
        write_exact_at(fd, hdr.entries_offset, entries.data(),
                       entries.size() * sizeof(NdtMetadataIndexEntry),
                       "NDT metadata index entries");
        write_exact_at(fd, hdr.extents_offset, extents.data(),
                       extents.size() * sizeof(NdtMetadataIndexExtent),
                       "NDT metadata index extents");
        write_exact_at(fd, hdr.lengths_offset, lengths.data(),
                       lengths.size() * sizeof(std::uint32_t),
                       "NDT metadata index lengths");
        if (::fdatasync(fd) != 0) {
            throw std::runtime_error(std::string("fdatasync metadata index failed: ") +
                                     std::strerror(errno));
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    (void)map_file_range_to_nvme_seg(temp_path, 0, hdr.index_bytes, max_extents);
    if (::rename(temp_path.c_str(), index_path.c_str()) != 0) {
        throw std::runtime_error(std::string("publish metadata index failed: ") +
                                 std::strerror(errno));
    }
    cleanup.path.clear();
    if (!try_load_arrow_metadata_index(input_path, index_path, in_segs,
                                       total_bytes, max_extents, verbose)) {
        throw std::runtime_error("published NDT metadata index failed validation");
    }
    if (cache_hit) {
        *cache_hit = false;
    }
    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] NDT metadata-only index entries=%zu records=%zu extents=%zu bytes=%llu\n",
                     entries.size(), lengths.size(), extents.size(),
                     static_cast<unsigned long long>(hdr.index_bytes));
    }
}

void build_input_segments(const std::string& input_path,
                          const std::string& output_path,
                          std::vector<NvmeSeg>& in_segs,
                          std::size_t& total_bytes,
                          std::size_t max_extents,
                          std::int64_t arrow_batch_start,
                          std::int64_t arrow_batch_count,
                          const std::string& requested_stage_path,
                          ArrowStageMode stage_mode,
                          std::string* stage_path,
                          bool* stage_cache_hit,
                          bool verbose) {

    (void)output_path;
    total_bytes = 0;
    in_segs.clear();
    if (stage_path) {
        stage_path->clear();
    }

    if (!has_arrow_extension(input_path)) {
        if (arrow_batch_start >= 0 || arrow_batch_count >= 0) {
            throw std::runtime_error("arrow batch range requires .arrow input");
        }
        fiemap_schedule::convert_fiemap_to_nvme_segs(input_path, in_segs, total_bytes);
        if (in_segs.empty()) {
            throw std::runtime_error("no input segments found");
        }
        return;
    }

    const std::string local_stage_path = requested_stage_path.empty()
        ? make_arrow_stage_metadata_path(input_path, arrow_batch_start, arrow_batch_count)
        : requested_stage_path;

    build_arrow_metadata_index(input_path,
                               local_stage_path,
                               in_segs,
                               total_bytes,
                               max_extents,
                               arrow_batch_start,
                               arrow_batch_count,
                               stage_mode,
                               stage_cache_hit,
                               verbose);

    if (stage_path) {
        *stage_path = local_stage_path;
    }
}

void* alloc_io_buf(std::size_t len) {
    if (len == 0) {
        return nullptr;
    }
    void* ptr = nullptr;
    const std::size_t align = 4096;
    if (posix_memalign(&ptr, align, len) != 0) {
        return nullptr;
    }
    return ptr;
}

void free_io_buf(void* ptr) {
    free(ptr);
}

void fill_nvme_cmd(std::uint8_t opcode,
                   std::uint32_t nsid,
                   std::uint64_t slba,
                   std::uint32_t in_nblocks,
                   std::uint32_t slot,
                   std::uint64_t out_slba,
                   std::uint32_t out_nblocks,
                   std::uint32_t payload_valid_bytes,
                   bool input_is_extent_descriptor,
                   bool input_is_metadata_index,
                   bool metadata_index_preload,
                   nvme_uring_cmd& out) {
    out = {};
    out.opcode = opcode;
    out.nsid = nsid;
    // Metadata-only command: the target owns the data buffer and performs
    // both input read and output write locally.
    out.addr = 0;
    out.data_len = 0;
    out.cdw10 = static_cast<std::uint32_t>(slba & 0xFFFFFFFFULL);
    out.cdw11 = static_cast<std::uint32_t>((slba >> 32) & 0xFFFFFFFFULL);
    out.cdw12 = (in_nblocks == 0) ? 0 : (in_nblocks - 1); // NLB-1
    if (input_is_extent_descriptor) {
        out.cdw12 |= kBpeExtentDescriptorCdw12Flag;
    }
    if (input_is_metadata_index) {
        out.cdw12 |= kBpeMetadataIndexCdw12Flag;
        out.cdw13 = slot;
        out.cdw14 = payload_valid_bytes;
        out.cdw15 = metadata_index_preload
            ? kBpeMetadataIndexPreload
            : kBpeMetadataIndexExecute;
        return;
    }
    out.cdw13 = slot;
    out.cdw14 = (out_nblocks == 0) ? payload_valid_bytes : static_cast<std::uint32_t>(out_slba & 0xFFFFFFFFULL);
    out.cdw15 = (out_nblocks == 0) ? 0xFFFFFFFFu : (out_nblocks - 1);
}

bool ensure_output_file(const std::string& path, std::size_t size_bytes, std::string& err) {
    struct stat before{};
    const bool reusable = (::stat(path.c_str(), &before) == 0 &&
                           before.st_size == static_cast<off_t>(size_bytes));
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = std::string("open failed: ") + std::strerror(errno);
        return false;
    }
    if (size_bytes > 0) {
        if (::ftruncate(fd, static_cast<off_t>(size_bytes)) != 0) {
            err = std::string("ftruncate failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }

        const long rc = ::syscall(SYS_fallocate, fd, 0, 0, static_cast<off_t>(size_bytes));
        if (rc != 0 && errno != EOPNOTSUPP && errno != ENOSYS && errno != EINVAL) {
            err = std::string("fallocate failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }

        // ext4 must convert new fallocated extents from unwritten to written
        // before SPDK updates their physical blocks.  Do this once when the
        // output pool is created or resized, then reuse it across runs.
        if (!reusable) {
            constexpr std::size_t kInitChunkBytes = 1U << 20;
            std::vector<char> zeros(std::min<std::size_t>(kInitChunkBytes, size_bytes), 0);
            std::size_t written = 0;
            while (written < size_bytes) {
                const std::size_t to_write = std::min<std::size_t>(zeros.size(), size_bytes - written);
                const ssize_t rc_write = ::pwrite(fd,
                                                  zeros.data(),
                                                  to_write,
                                                  static_cast<off_t>(written));
                if (rc_write < 0) {
                    err = std::string("pwrite init failed: ") + std::strerror(errno);
                    ::close(fd);
                    return false;
                }
                if (rc_write == 0) {
                    err = "pwrite init wrote 0 bytes";
                    ::close(fd);
                    return false;
                }
                written += static_cast<std::size_t>(rc_write);
            }
        }

        // The SPDK target writes the file's physical LBAs behind the host page
        // cache.  Flush all dirty host pages before publishing those LBAs,
        // including when an existing output allocation is reused.
        if (::fdatasync(fd) != 0) {
            err = std::string("fdatasync failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    return true;
}



bool drop_file_cache(const std::string& path, std::size_t size_bytes, std::string* err = nullptr) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) {
            *err = std::string("open(cache-drop) failed: ") + std::strerror(errno);
        }
        return false;
    }
    const int rc = ::posix_fadvise(fd, 0, static_cast<off_t>(size_bytes), POSIX_FADV_DONTNEED);
    if (rc != 0) {
        if (err) {
            *err = std::string("posix_fadvise(DONTNEED) failed: ") + std::strerror(rc);
        }
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

void prepare_metadata_output_routes(const std::string& input_path,
                                    const std::string& source_index_path,
                                    const std::string& output_path,
                                    std::size_t max_extents,
                                    std::vector<NvmeSeg>& command_index_segs,
                                    std::vector<NdtOutputRoute>& routes,
                                    std::vector<std::uint64_t>& output_offsets,
                                    std::size_t& output_pool_bytes) {
    const int source_fd = ::open(source_index_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) {
        throw std::runtime_error(std::string("open source NDT index failed: ") +
                                 std::strerror(errno));
    }
    NdtMetadataIndexHeader hdr{};
    std::vector<NdtMetadataIndexEntry> entries;
    std::vector<NdtMetadataIndexExtent> extents;
    std::vector<std::uint32_t> lengths;
    try {
        read_exact_at(source_fd, 0, &hdr, sizeof(hdr), "source NDT index header");
        if (hdr.magic != kNdtMetadataIndexMagic ||
            hdr.version != kNdtMetadataIndexVersion ||
            hdr.header_size != sizeof(hdr) ||
            hdr.entry_size != sizeof(NdtMetadataIndexEntry) ||
            hdr.extent_size != sizeof(NdtMetadataIndexExtent) ||
            !ndt_index_source_matches(input_path, hdr)) {
            throw std::runtime_error("source NDT metadata index is invalid or stale");
        }
        entries.resize(static_cast<std::size_t>(hdr.entry_count));
        extents.resize(static_cast<std::size_t>(hdr.extent_count));
        lengths.resize(static_cast<std::size_t>(hdr.record_count));
        read_exact_at(source_fd, hdr.entries_offset, entries.data(),
                      entries.size() * sizeof(entries.front()), "source NDT entries");
        read_exact_at(source_fd, hdr.extents_offset, extents.data(),
                      extents.size() * sizeof(extents.front()), "source NDT extents");
        read_exact_at(source_fd, hdr.lengths_offset, lengths.data(),
                      lengths.size() * sizeof(lengths.front()), "source NDT lengths");
    } catch (...) {
        ::close(source_fd);
        throw;
    }
    ::close(source_fd);

    routes.resize(entries.size());
    std::vector<NdtMetadataIndexExtent> output_extents;
    output_offsets.resize(entries.size());
    output_pool_bytes = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        // Byte-level BPE emits at most one int32 token per input byte.
        const std::uint64_t capacity = align_up_u64(
            std::max<std::uint64_t>(FIEMAP_LBA_BYTES,
                                    static_cast<std::uint64_t>(entries[i].payload_bytes) *
                                        sizeof(std::int32_t)),
            FIEMAP_LBA_BYTES);
        if (capacity > kArrowOutputPageBytes) {
            throw std::runtime_error("bounded BPE output capacity exceeds 512 KiB runtime limit");
        }
        output_offsets[i] = output_pool_bytes;
        output_pool_bytes += static_cast<std::size_t>(capacity);
    }

    std::string prepare_err;
    if (!ensure_output_file(output_path, output_pool_bytes, prepare_err)) {
        throw std::runtime_error("prepare NDT output allocation failed: " + prepare_err);
    }
    std::string cache_err;
    if (!drop_file_cache(output_path, output_pool_bytes, &cache_err)) {
        throw std::runtime_error("flush/invalidate NDT output before direct-LBA write failed: " +
                                 cache_err);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint64_t capacity = (i + 1 < entries.size())
            ? output_offsets[i + 1] - output_offsets[i]
            : output_pool_bytes - output_offsets[i];
        const auto segs = map_file_range_to_nvme_segs(
            output_path, output_offsets[i], capacity, max_extents);
        if (segs.empty() || segs.size() > 255) {
            throw std::runtime_error("invalid output extent count for bounded command");
        }
        routes[i].first_extent = static_cast<std::uint32_t>(output_extents.size());
        routes[i].extent_count = static_cast<std::uint32_t>(segs.size());
        routes[i].capacity_bytes = static_cast<std::uint32_t>(capacity);
        for (const auto& seg : segs) {
            output_extents.push_back(NdtMetadataIndexExtent{seg.slba, seg.nblocks, 0});
        }
    }

    const std::uint64_t length_bytes = hdr.record_count * sizeof(std::uint32_t);
    const std::uint64_t routes_offset = align_up_u64(hdr.lengths_offset + length_bytes, 8);
    hdr.reserved = kNdtOutputRoutesMagic;
    const std::uint64_t output_extents_offset =
        routes_offset + routes.size() * sizeof(NdtOutputRoute);
    hdr.index_bytes = align_up_u64(
        output_extents_offset + output_extents.size() * sizeof(NdtMetadataIndexExtent),
        FIEMAP_LBA_BYTES);
    const std::string routed_index_path = output_path + ".ndtidx";
    if (!ensure_output_file(routed_index_path, static_cast<std::size_t>(hdr.index_bytes),
                            prepare_err)) {
        throw std::runtime_error("prepare routed NDT index failed: " + prepare_err);
    }
    const int routed_fd = ::open(routed_index_path.c_str(), O_RDWR | O_CLOEXEC);
    if (routed_fd < 0) {
        throw std::runtime_error(std::string("open routed NDT index failed: ") +
                                 std::strerror(errno));
    }
    try {
        write_exact_at(routed_fd, 0, &hdr, sizeof(hdr), "routed NDT index header");
        write_exact_at(routed_fd, hdr.entries_offset, entries.data(),
                       entries.size() * sizeof(entries.front()), "routed NDT entries");
        write_exact_at(routed_fd, hdr.extents_offset, extents.data(),
                       extents.size() * sizeof(extents.front()), "routed NDT extents");
        write_exact_at(routed_fd, hdr.lengths_offset, lengths.data(),
                       lengths.size() * sizeof(lengths.front()), "routed NDT lengths");
        write_exact_at(routed_fd, routes_offset, routes.data(),
                       routes.size() * sizeof(routes.front()), "routed NDT output routes");
        write_exact_at(routed_fd, output_extents_offset, output_extents.data(),
                       output_extents.size() * sizeof(output_extents.front()),
                       "routed NDT output extents");
        if (::fdatasync(routed_fd) != 0) {
            throw std::runtime_error(std::string("fdatasync routed NDT index failed: ") +
                                     std::strerror(errno));
        }
    } catch (...) {
        ::close(routed_fd);
        throw;
    }
    ::close(routed_fd);
    if (!drop_file_cache(routed_index_path, static_cast<std::size_t>(hdr.index_bytes),
                         &cache_err)) {
        throw std::runtime_error("invalidate routed NDT index cache failed: " + cache_err);
    }
    const NvmeSeg routed_seg = map_file_range_to_nvme_seg(
        routed_index_path, 0, hdr.index_bytes, max_extents);
    command_index_segs.clear();
    command_index_segs.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        command_index_segs.push_back(NvmeSeg{routed_seg.slba, routed_seg.nblocks,
                                             static_cast<std::uint32_t>(i)});
    }
}

bool build_jobs(const std::vector<NvmeSeg>& in_segs,
                const std::vector<NvmeSeg>& out_segs,
                std::vector<IoJob>& jobs) {
    jobs.clear();
    if (in_segs.empty() || out_segs.empty()) {
        return false;
    }

    std::size_t in_idx = 0;
    std::size_t out_idx = 0;
    std::uint64_t in_lba = in_segs[0].slba;
    std::uint64_t out_lba = out_segs[0].slba;
    std::uint64_t in_rem = in_segs[0].nblocks;
    std::uint64_t out_rem = out_segs[0].nblocks;

    while (in_idx < in_segs.size()) {
        if (out_idx >= out_segs.size()) {
            return false;
        }
        if (in_rem == 0) {
            ++in_idx;
            if (in_idx >= in_segs.size()) break;
            in_lba = in_segs[in_idx].slba;
            in_rem = in_segs[in_idx].nblocks;
            continue;
        }
        if (out_rem == 0) {
            ++out_idx;
            if (out_idx >= out_segs.size()) break;
            out_lba = out_segs[out_idx].slba;
            out_rem = out_segs[out_idx].nblocks;
            continue;
        }

        const std::uint64_t chunk = std::min(in_rem, out_rem);
        jobs.push_back(IoJob{in_lba, out_lba, static_cast<std::uint32_t>(chunk),
                             static_cast<std::uint32_t>(chunk), jobs.size(), false});
        in_lba += chunk;
        out_lba += chunk;
        in_rem -= chunk;
        out_rem -= chunk;
    }

    return (in_idx >= in_segs.size() && in_rem == 0);
}

} // namespace

py::dict prepare_arrow_stage(const std::string& input_path,
                             const std::string& requested_stage_path,
                             bool force_rebuild,
                             std::size_t max_extents,
                             std::int64_t arrow_batch_start,
                             std::int64_t arrow_batch_count,
                             bool verbose) {
    if (!has_arrow_extension(input_path)) {
        throw std::invalid_argument("prepare_arrow_stage requires a .arrow input");
    }

    const std::string resolved_stage_path = requested_stage_path.empty()
        ? make_arrow_stage_metadata_path(input_path, arrow_batch_start, arrow_batch_count)
        : requested_stage_path;
    std::vector<NvmeSeg> segments;
    std::size_t total_bytes = 0;
    bool cache_hit = false;
    const double started_us = now_us();
    {
        py::gil_scoped_release release;
        build_arrow_metadata_index(input_path,
                                   resolved_stage_path,
                                   segments,
                                   total_bytes,
                                   max_extents,
                                   arrow_batch_start,
                                   arrow_batch_count,
                                   force_rebuild ? ArrowStageMode::Rebuild
                                                 : ArrowStageMode::Auto,
                                   &cache_hit,
                                   verbose);
    }

    struct stat stage_st {};
    if (::stat(resolved_stage_path.c_str(), &stage_st) != 0) {
        throw std::runtime_error(std::string("stat prepared stage failed: ") +
                                 std::strerror(errno));
    }

    py::dict result;
    result["input_path"] = input_path;
    result["stage_path"] = resolved_stage_path;
    result["stage_cache_hit"] = cache_hit;
    result["stage_format_version"] = kNdtMetadataIndexVersion;
    result["segments"] = segments.size();
    result["payload_read_bytes"] = total_bytes;
    result["stage_file_bytes"] = static_cast<std::uint64_t>(stage_st.st_size);
    result["prepare_us"] = now_us() - started_us;
    return result;
}

py::dict inspect_arrow_stage(const std::string& input_path,
                             const std::string& requested_stage_path,
                             std::size_t max_extents,
                             std::int64_t arrow_batch_start,
                             std::int64_t arrow_batch_count) {
    if (!has_arrow_extension(input_path)) {
        throw std::invalid_argument("inspect_arrow_stage requires a .arrow input");
    }
    const std::string resolved_stage_path = requested_stage_path.empty()
        ? make_arrow_stage_metadata_path(input_path, arrow_batch_start, arrow_batch_count)
        : requested_stage_path;
    std::vector<NvmeSeg> segments;
    std::size_t total_bytes = 0;
    bool valid = false;
    {
        py::gil_scoped_release release;
        ScopedStageLock stage_lock(resolved_stage_path);
        valid = try_load_arrow_metadata_index(input_path,
                                              resolved_stage_path,
                                              segments,
                                              total_bytes,
                                              max_extents,
                                              false);
    }

    struct stat stage_st {};
    const bool exists = ::stat(resolved_stage_path.c_str(), &stage_st) == 0;
    py::dict result;
    result["input_path"] = input_path;
    result["stage_path"] = resolved_stage_path;
    result["exists"] = exists;
    result["valid"] = valid;
    result["stage_format_version"] = kNdtMetadataIndexVersion;
    result["segments"] = segments.size();
    result["payload_read_bytes"] = total_bytes;
    result["stage_file_bytes"] = exists
        ? static_cast<std::uint64_t>(stage_st.st_size)
        : 0ULL;
    return result;
}

py::dict tokenize_to_nvme(const std::string& dev_path,
                          const std::string& input_path,
                          const std::string& output_path,
                          std::uint8_t opcode,
                          std::uint32_t nsid,
                          unsigned queue_depth,
                          std::size_t max_inflight,
                          std::size_t slots,
                          int fixed_slot,
                          std::size_t max_blocks_per_seg,
                          std::size_t max_extents,
                          std::int64_t arrow_batch_start,
                          std::int64_t arrow_batch_count,
                          std::uint64_t completion_timeout_us,
                          bool admin,
                          bool verbose,
                          const std::string& requested_stage_path,
                          const std::string& stage_mode_name) {
    const std::string out_path = output_path.empty() ? (input_path + ".bin") : output_path;
    const std::size_t inflight = (max_inflight == 0) ? slots : max_inflight;
    if (inflight == 0) {
        throw std::runtime_error("max_inflight must be > 0");
    }
    if (fixed_slot >= 0) {
        if (slots == 0) {
            throw std::runtime_error("fixed_slot requires slots > 0");
        }
        if (static_cast<std::size_t>(fixed_slot) >= slots) {
            throw std::runtime_error("fixed_slot must be smaller than slots");
        }
        if (inflight != 1) {
            throw std::runtime_error("fixed_slot mode requires max_inflight=1");
        }
    }

    std::size_t total_bytes = 0;
    std::size_t errors = 0;
    std::size_t segments = 0;
    std::uint64_t output_valid_bytes = 0;
    double elapsed_us = 0.0;
    double breakdown_segment_build_us = 0.0;
    double breakdown_output_prepare_us = 0.0;
    double breakdown_job_map_us = 0.0;
    double breakdown_dev_open_us = 0.0;
    double breakdown_ring_init_us = 0.0;
    double breakdown_index_preload_us = 0.0;
    double breakdown_sqe_prepare_us = 0.0;
    double breakdown_submit_us = 0.0;
    double breakdown_cqe_wait_us = 0.0;
    double breakdown_cqe_process_us = 0.0;
    double breakdown_manifest_us = 0.0;
    double breakdown_cache_drop_us = 0.0;
    double breakdown_close_us = 0.0;
    std::size_t breakdown_submit_calls = 0;
    std::size_t breakdown_completion_count = 0;
    std::string stage_path;
    bool stage_cache_hit = false;
    const ArrowStageMode stage_mode = parse_arrow_stage_mode(stage_mode_name);

    {
        py::gil_scoped_release release;

        std::vector<NvmeSeg> in_segs;
        double tb0 = now_us();
        if (has_arrow_extension(input_path)) {
            build_input_segments(input_path,
                                 out_path,
                                 in_segs,
                                 total_bytes,
                                 max_extents,
                                 arrow_batch_start,
                                 arrow_batch_count,
                                 requested_stage_path,
                                 stage_mode,
                                 &stage_path,
                                 &stage_cache_hit,
                                 verbose);
            // Keep the NDT-native staged representation.  Dataset ingestion
            // is a one-time preparation step; deleting it here caused every
            // benchmark run to retransmit the complete input through ext4.
        } else {
            fiemap_schedule::convert_fiemap_to_nvme_segs(input_path,
                                                         in_segs,
                                                         total_bytes,
                                                         max_blocks_per_seg,
                                                         max_extents);
            if (in_segs.empty()) {
                throw std::runtime_error("no input segments found");
            }
        }
        breakdown_segment_build_us += now_us() - tb0;

        tb0 = now_us();
        const bool metadata_index_input = has_arrow_extension(input_path);
        std::size_t output_pool_bytes = 0;
        std::vector<NdtOutputRoute> output_routes;
        std::vector<std::uint64_t> output_offsets;
        if (metadata_index_input) {
            prepare_metadata_output_routes(input_path, stage_path, out_path, max_extents,
                                           in_segs, output_routes, output_offsets,
                                           output_pool_bytes);
        }
        breakdown_output_prepare_us += now_us() - tb0;

        tb0 = now_us();
        std::vector<IoJob> jobs;
        jobs.reserve(in_segs.size());
        for (std::size_t i = 0; i < in_segs.size(); ++i) {
            jobs.push_back(IoJob{in_segs[i].slba, 0, in_segs[i].nblocks,
                                 0, in_segs[i].valid_bytes, i, false,
                                 metadata_index_input, metadata_index_input});
        }
        segments = jobs.size();
        std::vector<std::uint32_t> valid_output_bytes(jobs.size(), 0);
        breakdown_job_map_us += now_us() - tb0;

        tb0 = now_us();
        const int dev_fd = ::open(dev_path.c_str(), O_RDWR | O_CLOEXEC);
        if (dev_fd < 0) {
            throw std::runtime_error(std::string("open dev failed: ") + std::strerror(errno));
        }
        breakdown_dev_open_us += now_us() - tb0;

        tb0 = now_us();
        Ring ring;
        ring.init(queue_depth, true);
        breakdown_ring_init_us += now_us() - tb0;
        const std::uint32_t cmd_op = admin ? NVME_URING_CMD_ADMIN : NVME_URING_CMD_IO;
        const std::size_t submit_batch = std::max<std::size_t>(1, inflight / 2);

        if (metadata_index_input) {
            const double preload_started_us = now_us();
            io_uring_sqe* preload_sqe = io_uring_get_sqe(ring.raw());
            if (preload_sqe == nullptr) {
                ::close(dev_fd);
                throw std::runtime_error("failed to allocate metadata-index preload SQE");
            }
            nvme_uring_cmd preload_cmd{};
            fill_nvme_cmd(opcode, nsid, in_segs.front().slba,
                          in_segs.front().nblocks, 0, 0, 0, 0,
                          false, true, true, preload_cmd);
            std::memset(preload_sqe, 0, sizeof(*preload_sqe));
            preload_sqe->opcode = IORING_OP_URING_CMD;
            preload_sqe->fd = dev_fd;
            preload_sqe->cmd_op = cmd_op;
            preload_sqe->user_data = 0x4e445449ULL;
            std::memcpy(preload_sqe->cmd, &preload_cmd, sizeof(preload_cmd));
            const int submit_rc = io_uring_submit(ring.raw());
            if (submit_rc < 0) {
                ::close(dev_fd);
                throw std::runtime_error(std::string("metadata-index preload submit failed: ") +
                                         std::strerror(-submit_rc));
            }
            io_uring_cqe* preload_cqe = nullptr;
            int wait_rc = 0;
            if (completion_timeout_us == 0) {
                wait_rc = io_uring_wait_cqe(ring.raw(), &preload_cqe);
            } else {
                __kernel_timespec timeout{};
                timeout.tv_sec = completion_timeout_us / 1000000ULL;
                timeout.tv_nsec = (completion_timeout_us % 1000000ULL) * 1000ULL;
                wait_rc = io_uring_wait_cqe_timeout(ring.raw(), &preload_cqe, &timeout);
            }
            bool preload_status_error = false;
            if (wait_rc >= 0 && ring.cqe32_enabled()) {
                const auto* ext = reinterpret_cast<const std::uint32_t*>(
                    preload_cqe->big_cqe);
                preload_status_error = ((ext[3] >> 17) & 0xFFFFU) != 0;
            }
            if (wait_rc < 0 || preload_cqe == nullptr || preload_cqe->res < 0 ||
                preload_status_error) {
                if (preload_cqe != nullptr) {
                    io_uring_cqe_seen(ring.raw(), preload_cqe);
                }
                ::close(dev_fd);
                throw std::runtime_error("metadata-index preload command failed");
            }
            io_uring_cqe_seen(ring.raw(), preload_cqe);
            breakdown_index_preload_us += now_us() - preload_started_us;
        }

        std::unordered_map<std::uint64_t, PendingInfo> pending;
        pending.reserve(inflight);

        std::size_t inflight_cnt = 0;
        std::size_t queued = 0;
        std::uint64_t next_id = 1;
        std::uint32_t slot_rr = 0;
        const std::size_t scheduler_slots =
            fixed_slot >= 0 ? static_cast<std::size_t>(fixed_slot + 1) : std::max<std::size_t>(1, slots);
        std::vector<std::size_t> slot_inflight(scheduler_slots, 0);

        const double t0 = now_us();

        auto free_all_pending = [&]() {
            for (auto& kv : pending) {
                free_io_buf(kv.second.buf);
            }
            pending.clear();
        };

        auto submit_one = [&](const IoJob& job) -> int {
            const double ts0 = now_us();
            std::uint32_t slot = 0;
            if (fixed_slot >= 0) {
                slot = static_cast<std::uint32_t>(fixed_slot);
                if (slot_inflight[slot] != 0) {
                    return -EBUSY;
                }
            } else {
                bool found = false;
                for (std::size_t attempt = 0; attempt < scheduler_slots; ++attempt) {
                    const std::uint32_t candidate = static_cast<std::uint32_t>(
                        (slot_rr + attempt) % scheduler_slots);
                    if (slot_inflight[candidate] == 0) {
                        slot = candidate;
                        slot_rr = (candidate + 1) % scheduler_slots;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return -EBUSY;
                }
            }

            io_uring_sqe* sqe = io_uring_get_sqe(ring.raw());
            if (!sqe) {
                return -EAGAIN;
            }

            nvme_uring_cmd uc{};
            fill_nvme_cmd(opcode, nsid, job.in_slba, job.in_nblocks, slot,
                          job.out_slba, job.out_nblocks, job.payload_valid_bytes,
                          job.input_is_extent_descriptor,
                          job.input_is_metadata_index,
                          false,
                          uc);

            std::memset(sqe, 0, sizeof(*sqe));
            sqe->opcode = IORING_OP_URING_CMD;
            sqe->fd = dev_fd;
            sqe->cmd_op = cmd_op;
            sqe->user_data = next_id;
            std::memcpy(sqe->cmd, &uc, sizeof(uc));

            pending.emplace(next_id, PendingInfo{job.in_slba, job.out_slba,
                                                 job.in_nblocks, job.index, nullptr, 0, slot});
            ++slot_inflight[slot];
            ++next_id;
            ++queued;
            ++inflight_cnt;
            breakdown_sqe_prepare_us += now_us() - ts0;
            return 0;
        };

        auto submit_queued = [&]() -> bool {
            if (queued == 0) {
                return true;
            }
            const double ts0 = now_us();
            const int rc = io_uring_submit(ring.raw());
            breakdown_submit_us += now_us() - ts0;
            ++breakdown_submit_calls;
            if (rc < 0) {
                if (verbose) {
                    std::fprintf(stderr, "io_uring_submit failed: %s\n", std::strerror(-rc));
                }
                return false;
            }
            queued = 0;
            return true;
        };

        auto reap_one = [&]() -> bool {
            io_uring_cqe* cqe = nullptr;
            int rc = 0;
            const double tw0 = now_us();
            if (completion_timeout_us == 0) {
                rc = io_uring_wait_cqe(ring.raw(), &cqe);
            } else {
                __kernel_timespec ts{};
                ts.tv_sec = completion_timeout_us / 1000000ULL;
                ts.tv_nsec = (completion_timeout_us % 1000000ULL) * 1000ULL;
                rc = io_uring_wait_cqe_timeout(ring.raw(), &cqe, &ts);
            }
            breakdown_cqe_wait_us += now_us() - tw0;
            if (rc < 0) {
                if (rc == -ETIME) {
                    ++errors;
                    if (verbose) {
                        std::fprintf(stderr,
                                     "io_uring_wait_cqe timeout after %llu us\n",
                                     static_cast<unsigned long long>(completion_timeout_us));
                    }
                    return false;
                }
                if (verbose) {
                    std::fprintf(stderr, "io_uring_wait_cqe failed: %s\n", std::strerror(-rc));
                }
                return false;
            }

            const double tp0 = now_us();
            bool nvme_status_err = false;
            std::uint16_t status_field = 0;
            std::uint32_t dw0 = 0;
            std::uint32_t dw1 = 0;
            std::uint32_t dw2 = 0;
            std::uint32_t dw3 = 0;
            if (ring.cqe32_enabled()) {
                const auto* ext = reinterpret_cast<const std::uint32_t*>(cqe->big_cqe);
                dw0 = ext[0];
                dw1 = ext[1];
                dw2 = ext[2];
                dw3 = ext[3];
                status_field = static_cast<std::uint16_t>((dw3 >> 17) & 0xFFFF);
                nvme_status_err = (status_field != 0);
            }

            if (cqe->res < 0 || nvme_status_err) {
                ++errors;
                if (verbose) {
                    std::fprintf(stderr,
                                 "[ERR] user_data=%llu res=%d status=%u dw0=0x%x dw1=0x%x dw2=0x%x dw3=0x%x\n",
                                 static_cast<unsigned long long>(cqe->user_data),
                                 cqe->res,
                                 static_cast<unsigned>(status_field),
                                 dw0,
                                 dw1,
                                 dw2,
                                 dw3);
                }
            }

            const auto it = pending.find(cqe->user_data);
            if (it != pending.end()) {
                if (!nvme_status_err && cqe->res >= 0) {
                    valid_output_bytes[it->second.job_index] = dw0;
                }
                free_io_buf(it->second.buf);
                if (it->second.slot < slot_inflight.size() &&
                    slot_inflight[it->second.slot] > 0) {
                    --slot_inflight[it->second.slot];
                }
                pending.erase(it);
            }
            io_uring_cqe_seen(ring.raw(), cqe);
            if (inflight_cnt > 0) {
                --inflight_cnt;
            }
            breakdown_cqe_process_us += now_us() - tp0;
            ++breakdown_completion_count;
            return true;
        };

        bool ok = true;
        for (const auto& job : jobs) {
            while (inflight_cnt >= inflight) {
                if (!submit_queued()) { ok = false; break; }
                if (!reap_one()) { ok = false; break; }
            }
            if (!ok) break;

            int rc = submit_one(job);
            while (rc == -EAGAIN || rc == -EBUSY) {
                if (!submit_queued()) { ok = false; break; }
                if (!reap_one()) { ok = false; break; }
                rc = submit_one(job);
            }
            if (!ok) break;
            if (rc < 0) { ok = false; break; }

            if (queued >= submit_batch) {
                if (!submit_queued()) { ok = false; break; }
            }
        }

        if (ok && !submit_queued()) ok = false;
        while (ok && inflight_cnt > 0) {
            if (!reap_one()) { ok = false; break; }
        }

        free_all_pending();
        tb0 = now_us();
        ::close(dev_fd);
        breakdown_close_us += now_us() - tb0;

        if (!ok) {
            throw std::runtime_error("tokenize_to_nvme failed");
        }

        std::string cache_drop_err;
        tb0 = now_us();
        const std::string manifest_path = out_path + ".ndtmanifest";
        FILE* manifest = std::fopen(manifest_path.c_str(), "w");
        if (manifest == nullptr) {
            throw std::runtime_error("failed to create NDT output manifest");
        }
        std::fprintf(manifest, "NDTOUT\t2\t%zu\t%zu\n", output_pool_bytes,
                     valid_output_bytes.size());
        for (std::size_t i = 0; i < valid_output_bytes.size(); ++i) {
            const std::uint64_t offset = metadata_index_input ? output_offsets[i] : 0;
            const std::uint32_t capacity = metadata_index_input
                ? output_routes[i].capacity_bytes : 0;
            std::fprintf(manifest, "%zu\t%llu\t%u\t%u\n", i,
                         static_cast<unsigned long long>(offset), capacity,
                         valid_output_bytes[i]);
            output_valid_bytes += valid_output_bytes[i];
        }
        std::fclose(manifest);
        breakdown_manifest_us += now_us() - tb0;
        tb0 = now_us();
        struct stat output_st {};
        if (::stat(out_path.c_str(), &output_st) == 0 &&
            !drop_file_cache(out_path, output_pool_bytes, &cache_drop_err) && verbose) {
            std::fprintf(stderr, "[WARN] %s\n", cache_drop_err.c_str());
        }
        breakdown_cache_drop_us += now_us() - tb0;

        const double t1 = now_us();
        elapsed_us = t1 - t0;
    }

    py::dict result;
    result["segments"] = segments;
    result["total_bytes"] = total_bytes;
    result["errors"] = errors;
    result["output_valid_bytes"] = output_valid_bytes;
    result["tokens"] = output_valid_bytes / sizeof(std::int32_t);
    result["elapsed_us"] = elapsed_us;
    result["breakdown_segment_build_us"] = breakdown_segment_build_us;
    result["breakdown_output_prepare_us"] = breakdown_output_prepare_us;
    result["breakdown_job_map_us"] = breakdown_job_map_us;
    result["breakdown_dev_open_us"] = breakdown_dev_open_us;
    result["breakdown_ring_init_us"] = breakdown_ring_init_us;
    result["breakdown_index_preload_us"] = breakdown_index_preload_us;
    result["breakdown_sqe_prepare_us"] = breakdown_sqe_prepare_us;
    result["breakdown_submit_us"] = breakdown_submit_us;
    result["breakdown_cqe_wait_us"] = breakdown_cqe_wait_us;
    result["breakdown_cqe_process_us"] = breakdown_cqe_process_us;
    result["breakdown_manifest_us"] = breakdown_manifest_us;
    result["breakdown_cache_drop_us"] = breakdown_cache_drop_us;
    result["breakdown_close_us"] = breakdown_close_us;
    result["breakdown_submit_calls"] = breakdown_submit_calls;
    result["breakdown_completion_count"] = breakdown_completion_count;
    result["stage_path"] = stage_path;
    result["stage_mode"] = stage_mode_name;
    result["stage_cache_hit"] = stage_cache_hit;
    result["out_path"] = out_path;
    return result;
}

py::dict arrow_text_dump(const std::string& input_path,
                         const std::string& output_path,
                         const std::string& column,
                         const std::string& index_path,
                         const std::string& delimiter,
                         std::int64_t max_rows) {
    ArrowDumpStats stats{};
    std::string error;
    if (!DumpArrowText(input_path,
                       output_path,
                       column,
                       index_path,
                       delimiter,
                       max_rows,
                       &stats,
                       &error)) {
        throw std::runtime_error(error);
    }

    py::dict result;
    result["rows"] = stats.rows;
    result["output_bytes"] = stats.output_bytes;
    result["output_path"] = output_path;
    result["index_path"] = index_path;
    return result;
}

PYBIND11_MODULE(ndt_compute, m) {
    m.doc() = "NDT-BPE compute bindings (FIEMAP + NVMe io_uring).";

    m.def(
        "tokenize_to_nvme",
        &tokenize_to_nvme,
        py::arg("dev_path"),
        py::arg("input_path"),
        py::arg("output_path") = "",
        py::arg("opcode") = kDefaultOpcode,
        py::arg("nsid") = kDefaultNsid,
        py::arg("queue_depth") = kDefaultQueueDepth,
        py::arg("max_inflight") = 0,
        py::arg("slots") = 4,
        py::arg("fixed_slot") = -1,
        py::arg("max_blocks_per_seg") = FIEMAP_MDTS_BLOCKS,
        py::arg("max_extents") = FIEMAP_MAX_EXTENTS,
        py::arg("arrow_batch_start") = -1,
        py::arg("arrow_batch_count") = -1,
        py::arg("completion_timeout_us") = 30000000ULL,
        py::arg("admin") = false,
        py::arg("verbose") = false,
        py::arg("stage_path") = "",
        py::arg("stage_mode") = "auto",
        "Run FIEMAP -> NVMe io_uring submit pipeline. For Arrow input, optional "
        "arrow_batch_start/count stages only selected record batches. stage_mode is "
        "auto, require-prestaged, or rebuild. Returns stats dict."
    );

    m.def(
        "prepare_arrow_stage",
        &prepare_arrow_stage,
        py::arg("input_path"),
        py::arg("stage_path") = "",
        py::arg("force_rebuild") = false,
        py::arg("max_extents") = FIEMAP_MAX_EXTENTS,
        py::arg("arrow_batch_start") = -1,
        py::arg("arrow_batch_count") = -1,
        py::arg("verbose") = false,
        "Create or reuse a persistent Arrow payload stage and its physical-location index."
    );

    m.def(
        "inspect_arrow_stage",
        &inspect_arrow_stage,
        py::arg("input_path"),
        py::arg("stage_path") = "",
        py::arg("max_extents") = FIEMAP_MAX_EXTENTS,
        py::arg("arrow_batch_start") = -1,
        py::arg("arrow_batch_count") = -1,
        "Validate a persistent Arrow payload stage without rebuilding it."
    );

    m.def(
        "arrow_text_dump",
        &arrow_text_dump,
        py::arg("input_path"),
        py::arg("output_path"),
        py::arg("column") = "text",
        py::arg("index_path") = "",
        py::arg("delimiter") = "\n",
        py::arg("max_rows") = -1,
        "Dump Arrow IPC file column to a delimited text file. Returns stats dict."
    );
}
