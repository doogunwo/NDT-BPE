#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr std::size_t FIEMAP_LBA_BYTES = 512ULL;
constexpr std::size_t FIEMAP_MDTS_BYTES = (128ULL * 1024ULL); // 128KiB
constexpr std::size_t FIEMAP_MDTS_BLOCKS = (FIEMAP_MDTS_BYTES / FIEMAP_LBA_BYTES);
constexpr std::size_t FIEMAP_MAX_EXTENTS = 128ULL;

struct NvmeSeg {
    std::uint64_t slba = 0;    // start LBA (512B units)
    std::uint32_t nblocks = 0; // number of 512B blocks
    std::uint32_t valid_bytes = 0; // valid payload bytes when this segment is a staged text payload
};

class fiemap_schedule {
public:
    // 파일 extent를 조회해 NVMe passthru 스케줄용 세그먼트 배열로 변환.
    // out_length_bytes는 세그먼트 총 길이(바이트)이다.
    static void convert_fiemap_to_nvme_segs(const std::string& filepath,
                                            std::vector<NvmeSeg>& out_segs,
                                            std::size_t& out_length_bytes);

    // max_blocks_per_seg 기준으로 세그먼트를 분할한다.
    static void convert_fiemap_to_nvme_segs(const std::string& filepath,
                                            std::vector<NvmeSeg>& out_segs,
                                            std::size_t& out_length_bytes,
                                            std::size_t max_blocks_per_seg,
                                            std::size_t max_extents = FIEMAP_MAX_EXTENTS);
};
