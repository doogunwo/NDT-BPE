#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fiemap_schedule.h"
#include "io-uring.h"

constexpr std::size_t LBA_BYTES = 512ULL;
constexpr std::size_t ALIGN_BYTES = 4096ULL;
constexpr std::size_t MDTS_BYTES = (128ULL * 1024ULL);     // 128KiB
constexpr std::size_t MDTS_BLOCKS = (MDTS_BYTES / LBA_BYTES); // 256 blocks

// fallocate로 할당한 메타데이터 영역 관리 클래스
class ffilesystem {
public:
    ffilesystem() = default;
    ~ffilesystem();

    ffilesystem(const ffilesystem&) = delete;
    ffilesystem& operator=(const ffilesystem&) = delete;

    // 메타데이터 영역 할당
    void allocate_metadata(bool zero = true);
    // fallocate로 할당한 메타데이터 영역 해제
    void free_metadata() noexcept;

    // 메타데이터 영역 조정
    void advocate_metadata(std::size_t new_size, bool zero = true);
    
    // NvmeSeg -> NvmePassthruCmd 변환
    void convert_metadata_to_nvme_cmds(std::vector<NvmePassthruCmd>& out_cmds) const;

    // 파편화 방지를 위해 메타데이터 영역 재할당
    void reallocate_metadata(bool zero = true);

    // 메타데이터 저장
    void flush_metadata();

    // 메타데이터 로드
    void load_metadata();

    // fiemap으로 산출된 세그먼트를 설정
    void set_metadata_segments(std::vector<NvmeSeg> segs);

    void set_metadata_file_path(std::string path);
    const std::string& metadata_file_path() const noexcept;

    void store_blob(const void* data, std::size_t len, bool zero = true);
    std::vector<std::uint8_t> load_blob_copy() const;

private:
    int metadata_fd_ = -1; // 파일 디스크립터
    void* metadata_ptr_ = nullptr; // fallocate로 할당한 메타데이터 영역 포인터
    std::size_t metadata_size_ = 0; // 할당된 메타데이터 영역 크기
    std::vector<NvmeSeg> metadata_segs_; // 메타데이터 영역의 NvmeSeg 배열
    std::string metadata_file_path_ = "metadata_area.dat";
};
