#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>

struct FileByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct LbaExtent {
    std::uint64_t slba = 0;
    std::uint32_t nblocks = 0;
};

struct TextBufferExtent {
    std::int64_t batch_index = 0;
    std::int64_t num_rows = 0;
    FileByteRange data_range{};
    std::vector<LbaExtent> lba_extents;
};

struct ExtentIndex {
    std::string arrow_path;
    std::string column;
    std::vector<TextBufferExtent> buffers;
};

struct ExtentIndexOptions {
    std::string column = "text";
    std::int64_t max_rows = -1;
    std::size_t max_extents = 128;
};

// Build extent index for Arrow IPC (file or stream).
// Maps text column value buffer byte-ranges -> LBA extents using FIEMAP.
// NOTE: Requires Arrow headers/libs (from pyarrow) at build time.
bool BuildArrowTextExtentIndex(const std::string& arrow_path,
                               const ExtentIndexOptions& options,
                               ExtentIndex* out_index,
                               std::string* error);

bool SerializeExtentIndexBinary(const ExtentIndex& index,
                                std::vector<std::uint8_t>* out_bytes,
                                std::string* error);

bool DeserializeExtentIndexBinary(const void* data,
                                  std::size_t size,
                                  ExtentIndex* out_index,
                                  std::string* error);
