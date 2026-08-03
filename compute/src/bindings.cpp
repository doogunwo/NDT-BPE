#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector, std::string 자동 변환
#include <pybind11/functional.h>

#include "arrow_text_dump.h"
#include "extent-index.h"
#include "fallocate.h"
#include "io-uring.h"       // Ring, submit_nvme_passthru
#include "fiemap_schedule.h" // convert_fiemap_to_nvme_segs

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
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
constexpr std::size_t kArrowChunkBytes = 128ULL * 1024ULL;
constexpr std::size_t kArrowChunkStrideBytes = kArrowChunkBytes * 2ULL;

struct __attribute__((packed)) ArrowChunkHeader {
    std::uint32_t magic = kArrowChunkMagic;
    std::uint16_t version = 1;
    std::uint16_t reserved = 0;
    std::uint32_t payload_offset = 0;
    std::uint32_t payload_length = 0;
    std::uint64_t data_range_offset = 0;
    std::int64_t batch_index = -1;
    std::int64_t num_rows = -1;
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
    std::uint32_t nblocks;
};

struct PendingInfo {
    std::uint64_t slba;
    std::uint64_t out_slba;
    std::uint32_t nblocks;
    void* buf;
    std::size_t buf_len;
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
constexpr std::uint32_t kArrowStageManifestVersion = 2;

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
    std::uint64_t slba = 0;
    std::uint64_t stage_offset = 0;
    std::uint64_t data_range_offset = 0;
    std::uint64_t payload_length = 0;
    std::int64_t batch_index = -1;
    std::int64_t num_rows = -1;
    std::uint32_t nblocks = 0;
    std::uint32_t reserved = 0;
};

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
        return input_path + ".metadata";
    }
    return input_path + ".batch" + std::to_string(arrow_batch_start) +
           ".count" + std::to_string(arrow_batch_count) + ".metadata";
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

NvmeSeg map_file_range_to_nvme_seg(const std::string& filepath,
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
    for (std::uint32_t i = 0; i < fm->fm_mapped_extents; ++i) {
        const auto& ext = extents[i];
        const std::uint64_t ext_start = ext.fe_logical;
        const std::uint64_t ext_end = ext.fe_logical + ext.fe_length;
        if (ext_start <= file_offset && ext_end >= end) {
            const std::uint64_t physical = ext.fe_physical + (file_offset - ext_start);
            if (physical % FIEMAP_LBA_BYTES != 0 || length % FIEMAP_LBA_BYTES != 0) {
                throw std::runtime_error("arrow stage range is not LBA aligned");
            }
            return NvmeSeg{physical / FIEMAP_LBA_BYTES,
                           static_cast<std::uint32_t>(length / FIEMAP_LBA_BYTES)};
        }
    }

    throw std::runtime_error("arrow stage range is not physically contiguous");
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
        if (entry.nblocks != FIEMAP_MDTS_BLOCKS ||
            entry.stage_offset < hdr.payload_base_offset ||
            (entry.stage_offset - hdr.payload_base_offset) % kArrowChunkStrideBytes != 0 ||
            entry.stage_offset + kArrowChunkBytes > static_cast<std::uint64_t>(stage_st.st_size) ||
            entry.payload_length > kArrowChunkBytes - sizeof(ArrowChunkHeader)) {
            in_segs.clear();
            total_bytes = 0;
            return false;
        }
        in_segs.push_back(NvmeSeg{entry.slba, entry.nblocks});
        total_bytes += static_cast<std::size_t>(entry.nblocks) * FIEMAP_LBA_BYTES;
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

void build_arrow_stage_file(const std::string& input_path,
                            const std::string& stage_path,
                            std::vector<NvmeSeg>& in_segs,
                            std::size_t& total_bytes,
                            std::size_t max_extents,
                            std::int64_t arrow_batch_start,
                            std::int64_t arrow_batch_count,
                            bool verbose) {
    if (try_load_arrow_stage_metadata(input_path,
                                      stage_path,
                                      in_segs,
                                      total_bytes,
                                      max_extents,
                                      verbose)) {
        return;
    }

    ExtentIndexOptions options;
    options.column = "text";
    options.max_rows = -1;
    options.max_extents = FIEMAP_MAX_EXTENTS;

    ExtentIndex index;
    if (!try_load_cached_extent_index(input_path, &index, verbose)) {
        std::string error;
        if (!BuildArrowTextExtentIndex(input_path, options, &index, &error)) {
            throw std::runtime_error("extent-index build failed: " + error);
        }
        save_cached_extent_index(input_path, index, verbose);
    }

    constexpr std::size_t kArrowChunkPayloadBytes =
        kArrowChunkBytes - sizeof(ArrowChunkHeader);
    const std::int64_t effective_start =
        (arrow_batch_start < 0) ? 0 : arrow_batch_start;
    const std::int64_t effective_end =
        (arrow_batch_count < 0)
            ? std::numeric_limits<std::int64_t>::max()
            : effective_start + arrow_batch_count;

    std::size_t planned_chunks = 0;
    for (const auto& buf : index.buffers) {
        if (buf.batch_index < effective_start || buf.batch_index >= effective_end) {
            continue;
        }
        planned_chunks += static_cast<std::size_t>(
            div_round_up_u64(buf.data_range.length, kArrowChunkPayloadBytes));
    }
    if (planned_chunks == 0) {
        throw std::runtime_error("no arrow text buffers selected for staging");
    }

    const std::uint64_t manifest_bytes =
        sizeof(ArrowStageManifestHeader) +
        static_cast<std::uint64_t>(planned_chunks) * sizeof(ArrowStageManifestEntry);
    const std::uint64_t payload_base_offset =
        align_up_u64(manifest_bytes, kArrowChunkStrideBytes);

    const int input_fd = ::open(input_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (input_fd < 0) {
        throw std::runtime_error(std::string("open input failed: ") + std::strerror(errno));
    }
    const int stage_fd = ::open(stage_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (stage_fd < 0) {
        const int saved = errno;
        ::close(input_fd);
        throw std::runtime_error(std::string("open stage failed: ") + std::strerror(saved));
    }

    std::size_t chunk_count = 0;
    std::vector<ArrowStageManifestEntry> entries;
    entries.reserve(planned_chunks);
    std::vector<char> chunk(kArrowChunkBytes, 0);

    try {
        for (const auto& buf : index.buffers) {
            if (buf.batch_index < effective_start || buf.batch_index >= effective_end) {
                continue;
            }

            std::uint64_t offset = buf.data_range.offset;
            std::uint64_t remaining = buf.data_range.length;
            while (remaining > 0) {
                const std::size_t payload_len = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, kArrowChunkPayloadBytes));
                std::fill(chunk.begin(), chunk.end(), 0);

                ArrowChunkHeader hdr{};
                hdr.payload_offset = static_cast<std::uint32_t>(sizeof(ArrowChunkHeader));
                hdr.payload_length = static_cast<std::uint32_t>(payload_len);
                hdr.data_range_offset = offset;
                hdr.batch_index = buf.batch_index;
                hdr.num_rows = buf.num_rows;

                std::memcpy(chunk.data(), &hdr, sizeof(hdr));
                read_exact_at(input_fd,
                              offset,
                              chunk.data() + sizeof(ArrowChunkHeader),
                              payload_len,
                              "arrow payload");

                const std::uint64_t stage_off =
                    payload_base_offset +
                    static_cast<std::uint64_t>(chunk_count) * kArrowChunkStrideBytes;
                write_exact_at(stage_fd,
                               stage_off,
                               chunk.data(),
                               chunk.size(),
                               "arrow stage");

                ArrowStageManifestEntry entry{};
                entry.stage_offset = stage_off;
                entry.data_range_offset = offset;
                entry.payload_length = payload_len;
                entry.batch_index = buf.batch_index;
                entry.num_rows = buf.num_rows;
                entries.push_back(entry);

                ++chunk_count;
                offset += payload_len;
                remaining -= payload_len;
            }
        }

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
            const NvmeSeg seg =
                map_file_range_to_nvme_seg(stage_path,
                                           entry.stage_offset,
                                           kArrowChunkBytes,
                                           max_extents);
            if (seg.nblocks != FIEMAP_MDTS_BLOCKS) {
                throw std::runtime_error("arrow stage segment size mismatch");
            }
            entry.slba = seg.slba;
            entry.nblocks = seg.nblocks;
            in_segs.push_back(seg);
            total_bytes += static_cast<std::size_t>(seg.nblocks) * FIEMAP_LBA_BYTES;
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
        ::close(input_fd);
        throw;
    }

    ::close(stage_fd);
    ::close(input_fd);

    if (verbose) {
        std::fprintf(stderr,
                     "[INFO] extent-index buffers=%zu staged_chunks=%zu segments=%zu metadata=%s manifest_bytes=%llu payload_base=%llu\n",
                     index.buffers.size(),
                     chunk_count,
                     in_segs.size(),
                     stage_path.c_str(),
                     static_cast<unsigned long long>(manifest_bytes),
                     static_cast<unsigned long long>(payload_base_offset));
    }
}

void build_input_segments(const std::string& input_path,
                          const std::string& output_path,
                          std::vector<NvmeSeg>& in_segs,
                          std::size_t& total_bytes,
                          std::size_t max_extents,
                          std::int64_t arrow_batch_start,
                          std::int64_t arrow_batch_count,
                          std::string* stage_path,
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

    const std::string local_stage_path =
        make_arrow_stage_metadata_path(input_path, arrow_batch_start, arrow_batch_count);

    build_arrow_stage_file(input_path,
                           local_stage_path,
                           in_segs,
                           total_bytes,
                           max_extents,
                           arrow_batch_start,
                           arrow_batch_count,
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
                   std::uint32_t nblocks,
                   std::uint32_t slot,
                   std::uint64_t out_slba,
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
    out.cdw12 = (nblocks == 0) ? 0 : (nblocks - 1); // NLB-1
    out.cdw13 = slot;
    out.cdw14 = static_cast<std::uint32_t>(out_slba & 0xFFFFFFFFULL);
    out.cdw15 = (nblocks == 0) ? 0 : (nblocks - 1);
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
        if (reusable) {
            ::close(fd);
            return true;
        }
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
        jobs.push_back(IoJob{in_lba, out_lba, static_cast<std::uint32_t>(chunk)});
        in_lba += chunk;
        out_lba += chunk;
        in_rem -= chunk;
        out_rem -= chunk;
    }

    return (in_idx >= in_segs.size() && in_rem == 0);
}

} // namespace

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
                          bool verbose) {
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
    double elapsed_us = 0.0;
    std::string stage_path;

    {
        py::gil_scoped_release release;

        std::vector<NvmeSeg> in_segs;
        if (has_arrow_extension(input_path)) {
            build_input_segments(input_path,
                                 out_path,
                                 in_segs,
                                 total_bytes,
                                 max_extents,
                                 arrow_batch_start,
                                 arrow_batch_count,
                                 &stage_path,
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

        std::string err;
        if (!ensure_output_file(out_path, total_bytes, err)) {
            throw std::runtime_error(err);
        }

        std::vector<NvmeSeg> out_segs;
        std::size_t out_bytes = 0;
        fiemap_schedule::convert_fiemap_to_nvme_segs(out_path,
                                                     out_segs,
                                                     out_bytes,
                                                     max_blocks_per_seg,
                                                     max_extents);
        if (out_bytes < total_bytes) {
            throw std::runtime_error("output file too small for input");
        }

        std::vector<IoJob> jobs;
        if (!build_jobs(in_segs, out_segs, jobs)) {
            throw std::runtime_error("failed to map input to output segments");
        }
        segments = jobs.size();

        const int dev_fd = ::open(dev_path.c_str(), O_RDWR | O_CLOEXEC);
        if (dev_fd < 0) {
            throw std::runtime_error(std::string("open dev failed: ") + std::strerror(errno));
        }

        Ring ring;
        ring.init(queue_depth, true);
        const std::uint32_t cmd_op = admin ? NVME_URING_CMD_ADMIN : NVME_URING_CMD_IO;
        const std::size_t submit_batch = std::max<std::size_t>(1, inflight / 2);

        std::unordered_map<std::uint64_t, PendingInfo> pending;
        pending.reserve(inflight);

        std::size_t inflight_cnt = 0;
        std::size_t queued = 0;
        std::uint64_t next_id = 1;
        std::uint32_t slot_rr = 0;

        const double t0 = now_us();

        auto free_all_pending = [&]() {
            for (auto& kv : pending) {
                free_io_buf(kv.second.buf);
            }
            pending.clear();
        };

        auto submit_one = [&](const IoJob& job) -> int {
            io_uring_sqe* sqe = io_uring_get_sqe(ring.raw());
            if (!sqe) {
                return -EAGAIN;
            }

            std::uint32_t slot = 0;
            if (fixed_slot >= 0) {
                slot = static_cast<std::uint32_t>(fixed_slot);
            } else {
                slot = static_cast<std::uint32_t>(
                    slots == 0 ? 0 : (slot_rr % slots));
                ++slot_rr;
            }

            nvme_uring_cmd uc{};
            fill_nvme_cmd(opcode, nsid, job.in_slba, job.nblocks, slot, job.out_slba, uc);

            std::memset(sqe, 0, sizeof(*sqe));
            sqe->opcode = IORING_OP_URING_CMD;
            sqe->fd = dev_fd;
            sqe->cmd_op = cmd_op;
            sqe->user_data = next_id;
            std::memcpy(sqe->cmd, &uc, sizeof(uc));

            pending.emplace(next_id, PendingInfo{job.in_slba, job.out_slba, job.nblocks, nullptr, 0});
            ++next_id;
            ++queued;
            ++inflight_cnt;
            return 0;
        };

        auto submit_queued = [&]() -> bool {
            if (queued == 0) {
                return true;
            }
            const int rc = io_uring_submit(ring.raw());
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
            if (completion_timeout_us == 0) {
                rc = io_uring_wait_cqe(ring.raw(), &cqe);
            } else {
                __kernel_timespec ts{};
                ts.tv_sec = completion_timeout_us / 1000000ULL;
                ts.tv_nsec = (completion_timeout_us % 1000000ULL) * 1000ULL;
                rc = io_uring_wait_cqe_timeout(ring.raw(), &cqe, &ts);
            }
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
                free_io_buf(it->second.buf);
                pending.erase(it);
            }
            io_uring_cqe_seen(ring.raw(), cqe);
            if (inflight_cnt > 0) {
                --inflight_cnt;
            }
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
            if (rc == -EAGAIN) {
                if (!submit_queued()) { ok = false; break; }
                if (!reap_one()) { ok = false; break; }
                rc = submit_one(job);
            }
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
        ::close(dev_fd);

        if (!ok) {
            throw std::runtime_error("tokenize_to_nvme failed");
        }

        std::string cache_drop_err;
        if (!drop_file_cache(out_path, total_bytes, &cache_drop_err) && verbose) {
            std::fprintf(stderr, "[WARN] %s\n", cache_drop_err.c_str());
        }

        const double t1 = now_us();
        elapsed_us = t1 - t0;
    }

    py::dict result;
    result["segments"] = segments;
    result["total_bytes"] = total_bytes;
    result["errors"] = errors;
    result["elapsed_us"] = elapsed_us;
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
        "Run FIEMAP -> NVMe io_uring submit pipeline. For Arrow input, optional "
        "arrow_batch_start/count stages only selected record batches. Returns stats dict."
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
