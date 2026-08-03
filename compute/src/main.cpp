// Minimal compute-side CLI: FIEMAP -> NVMe uring_cmd submission.
#include "fiemap_schedule.h"
#include "io-uring.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

constexpr std::uint8_t kDefaultOpcode = 0xD4;
constexpr std::uint32_t kDefaultNsid = 1;
constexpr unsigned kDefaultQueueDepth = 256;

double now_us() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) / 1e3;
}

std::string decode_nvme_status_field(std::uint16_t status_field) {
    if (status_field == 0) {
        return "SUCCESS";
    }
    const std::uint32_t sc = status_field & 0xFF;
    const std::uint32_t sct = (status_field >> 8) & 0x7;
    const std::uint32_t more = (status_field >> 14) & 0x1;
    const std::uint32_t dnr = (status_field >> 15) & 0x1;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "SCT=%u SC=0x%02X M=%u DNR=%u", sct, sc, more, dnr);
    return std::string(buf);
}

struct Options {
    std::string dev_path;
    std::string file_path;
    std::string out_path;
    std::uint8_t opcode = kDefaultOpcode;
    std::uint32_t nsid = kDefaultNsid;
    unsigned queue_depth = kDefaultQueueDepth;
    std::size_t max_inflight = 0;
    std::size_t slots = 5;
    int fixed_slot = -1;
    std::size_t max_blocks_per_seg = FIEMAP_MDTS_BLOCKS;
    std::size_t max_extents = FIEMAP_MAX_EXTENTS;
    bool admin = false;
    bool quiet = false;
    bool max_inflight_set = false;
};

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " <nvme_dev> <file_path> [options]\n"
        << "Options:\n"
        << "  --opcode <hex|dec>         NVMe opcode (default 0xD4)\n"
        << "  --nsid <n>                 Namespace ID (default 1)\n"
        << "  --queue-depth <n>          io_uring queue depth (default 256)\n"
        << "  --max-inflight <n>         Max in-flight commands (default slots)\n"
        << "  --slots <n>                Slot count for cdw13 round-robin (default 5)\n"
        << "  --fixed-slot <n>           Use only one slot id (requires max-inflight=1)\n"
        << "  --out-file <path>          Output file path (default: <file_path>.bin)\n"
        << "  --max-blocks-per-seg <n>   Split segments by N blocks (default 256)\n"
        << "  --max-extents <n>          Max FIEMAP extents (default 128)\n"
        << "  --admin                    Use NVME_URING_CMD_ADMIN (default IO)\n"
        << "  --quiet                    Reduce output\n"
        << "  --help                     Show this message\n";
}

bool parse_u64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    std::uint64_t v = std::strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

bool parse_args(int argc, char** argv, Options& opt) {
    if (argc < 3) {
        return false;
    }

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            return false;
        } else if (arg == "--opcode" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v > 0xFF) {
                std::cerr << "Invalid opcode\n";
                return false;
            }
            opt.opcode = static_cast<std::uint8_t>(v);
        } else if (arg == "--nsid" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v == 0 || v > 0xFFFFFFFFu) {
                std::cerr << "Invalid nsid\n";
                return false;
            }
            opt.nsid = static_cast<std::uint32_t>(v);
        } else if (arg == "--queue-depth" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v == 0 || v > 65535) {
                std::cerr << "Invalid queue depth\n";
                return false;
            }
            opt.queue_depth = static_cast<unsigned>(v);
        } else if (arg == "--max-inflight" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v == 0) {
                std::cerr << "Invalid max-inflight\n";
                return false;
            }
            opt.max_inflight = static_cast<std::size_t>(v);
            opt.max_inflight_set = true;
        } else if (arg == "--slots" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v == 0) {
                std::cerr << "Invalid slots\n";
                return false;
            }
            opt.slots = static_cast<std::size_t>(v);
        } else if (arg == "--fixed-slot" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v)) {
                std::cerr << "Invalid fixed-slot\n";
                return false;
            }
            opt.fixed_slot = static_cast<int>(v);
        } else if (arg == "--out-file" && i + 1 < argc) {
            opt.out_path = argv[++i];
        } else if (arg == "--max-blocks-per-seg" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v == 0) {
                std::cerr << "Invalid max-blocks-per-seg\n";
                return false;
            }
            opt.max_blocks_per_seg = static_cast<std::size_t>(v);
        } else if (arg == "--max-extents" && i + 1 < argc) {
            std::uint64_t v = 0;
            if (!parse_u64(argv[++i], v) || v == 0) {
                std::cerr << "Invalid max-extents\n";
                return false;
            }
            opt.max_extents = static_cast<std::size_t>(v);
        } else if (arg == "--admin") {
            opt.admin = true;
        } else if (arg == "--quiet") {
            opt.quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        } else {
            if (positional == 0) {
                opt.dev_path = arg;
            } else if (positional == 1) {
                opt.file_path = arg;
            } else {
                std::cerr << "Too many positional arguments\n";
                return false;
            }
            ++positional;
        }
    }

    if (opt.dev_path.empty() || opt.file_path.empty()) {
        return false;
    }
    if (!opt.max_inflight_set) {
        opt.max_inflight = opt.slots;
    }
    if (opt.fixed_slot >= 0) {
        if (opt.slots == 0 || static_cast<std::size_t>(opt.fixed_slot) >= opt.slots) {
            std::cerr << "fixed-slot must be smaller than slots\n";
            return false;
        }
        if (opt.max_inflight != 1) {
            std::cerr << "fixed-slot requires max-inflight=1\n";
            return false;
        }
    }
    if (opt.out_path.empty()) {
        opt.out_path = opt.file_path + ".bin";
    }
    return true;
}

struct PendingInfo {
    std::uint64_t slba;
    std::uint64_t out_slba;
    std::uint32_t nblocks;
    void* buf;
    std::size_t buf_len;
};

struct IoJob {
    std::uint64_t in_slba;
    std::uint64_t out_slba;
    std::uint32_t nblocks;
};

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
                   void* data,
                   std::uint32_t data_len,
                   nvme_uring_cmd& out) {
    out = {};
    out.opcode = opcode;
    out.nsid = nsid;
    out.addr = reinterpret_cast<std::uint64_t>(data);
    out.data_len = data_len;
    out.cdw10 = static_cast<std::uint32_t>(slba & 0xFFFFFFFFULL);
    out.cdw11 = static_cast<std::uint32_t>((slba >> 32) & 0xFFFFFFFFULL);
    out.cdw12 = (nblocks == 0) ? 0 : (nblocks - 1); // NLB-1
    out.cdw13 = slot;
    out.cdw14 = static_cast<std::uint32_t>(out_slba & 0xFFFFFFFFULL);
    out.cdw15 = (nblocks == 0) ? 0 : (nblocks - 1);
}

int open_dev(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "open(" << path << ") failed: " << std::strerror(errno) << "\n";
    }
    return fd;
}

bool ensure_output_file(const std::string& path, std::size_t size_bytes) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::cerr << "open(" << path << ") failed: " << std::strerror(errno) << "\n";
        return false;
    }
    if (size_bytes > 0) {
        if (::ftruncate(fd, static_cast<off_t>(size_bytes)) != 0) {
            std::cerr << "ftruncate(" << path << ") failed: " << std::strerror(errno) << "\n";
            ::close(fd);
            return false;
        }

        std::vector<char> zeros(1U << 20, 0);
        std::size_t written = 0;
        while (written < size_bytes) {
            const std::size_t chunk = std::min<std::size_t>(zeros.size(), size_bytes - written);
            const ssize_t rc = ::pwrite(fd,
                                        zeros.data(),
                                        chunk,
                                        static_cast<off_t>(written));
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "pwrite(" << path << ") failed: " << std::strerror(errno) << "\n";
                ::close(fd);
                return false;
            }
            written += static_cast<std::size_t>(rc);
        }

        if (::fsync(fd) != 0) {
            std::cerr << "fsync(" << path << ") failed: " << std::strerror(errno) << "\n";
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

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage(argv[0]);
        return 1;
    }

    std::vector<NvmeSeg> segs;
    std::size_t total_bytes = 0;
    try {
        fiemap_schedule::convert_fiemap_to_nvme_segs(opt.file_path,
                                                     segs,
                                                     total_bytes,
                                                     opt.max_blocks_per_seg,
                                                     opt.max_extents);
    } catch (const std::exception& e) {
        std::cerr << "fiemap failed: " << e.what() << "\n";
        return 1;
    }

    if (segs.empty()) {
        std::cerr << "No segments found\n";
        return 1;
    }

    if (!ensure_output_file(opt.out_path, total_bytes)) {
        return 1;
    }

    std::vector<NvmeSeg> out_segs;
    std::size_t out_bytes = 0;
    try {
        fiemap_schedule::convert_fiemap_to_nvme_segs(opt.out_path,
                                                     out_segs,
                                                     out_bytes,
                                                     opt.max_blocks_per_seg,
                                                     opt.max_extents);
    } catch (const std::exception& e) {
        std::cerr << "fiemap(output) failed: " << e.what() << "\n";
        return 1;
    }

    if (out_bytes < total_bytes) {
        std::cerr << "output file too small: out_bytes=" << out_bytes
                  << " total_bytes=" << total_bytes << "\n";
        return 1;
    }

    std::vector<IoJob> jobs;
    if (!build_jobs(segs, out_segs, jobs)) {
        std::cerr << "failed to map input to output segments\n";
        return 1;
    }

    const int dev_fd = open_dev(opt.dev_path);
    if (dev_fd < 0) {
        return 1;
    }

    Ring ring;
    try {
        ring.init(opt.queue_depth, true);
    } catch (const std::exception& e) {
        std::cerr << "io_uring init failed: " << e.what() << "\n";
        ::close(dev_fd);
        return 1;
    }

    const std::uint32_t cmd_op = opt.admin ? NVME_URING_CMD_ADMIN : NVME_URING_CMD_IO;
    const std::size_t submit_batch = std::max<std::size_t>(1, opt.max_inflight / 2);

    std::unordered_map<std::uint64_t, PendingInfo> pending;
    pending.reserve(opt.max_inflight);

    std::size_t inflight = 0;
    std::size_t queued = 0;
    std::uint64_t next_id = 1;
    std::uint32_t slot_rr = 0;
    std::size_t errors = 0;

    const double t0 = now_us();

    auto submit_one = [&](const IoJob& job) -> int {
        io_uring_sqe* sqe = io_uring_get_sqe(ring.raw());
        if (!sqe) {
            return -EAGAIN;
        }

        const std::size_t buf_len = static_cast<std::size_t>(job.nblocks) * FIEMAP_LBA_BYTES;
        void* buf = alloc_io_buf(buf_len);
        if (buf == nullptr) {
            return -ENOMEM;
        }

        std::uint32_t slot = 0;
        if (opt.fixed_slot >= 0) {
            slot = static_cast<std::uint32_t>(opt.fixed_slot);
        } else {
            slot = static_cast<std::uint32_t>(
                opt.slots == 0 ? 0 : (slot_rr % opt.slots));
            ++slot_rr;
        }

        nvme_uring_cmd uc{};
        fill_nvme_cmd(opt.opcode, opt.nsid, job.in_slba, job.nblocks, slot, job.out_slba,
                      buf, static_cast<std::uint32_t>(buf_len), uc);

        std::memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_URING_CMD;
        sqe->fd = dev_fd;
        sqe->cmd_op = cmd_op;
        sqe->user_data = next_id;
        std::memcpy(sqe->cmd, &uc, sizeof(uc));

        pending.emplace(next_id, PendingInfo{job.in_slba, job.out_slba, job.nblocks, buf, buf_len});
        ++next_id;
        ++queued;
        ++inflight;
        return 0;
    };

    auto submit_queued = [&]() -> bool {
        if (queued == 0) {
            return true;
        }
        const int rc = io_uring_submit(ring.raw());
        if (rc < 0) {
            std::cerr << "io_uring_submit failed: " << std::strerror(-rc) << "\n";
            return false;
        }
        queued = 0;
        return true;
    };

    auto reap_one = [&]() -> bool {
        io_uring_cqe* cqe = nullptr;
        const int rc = io_uring_wait_cqe(ring.raw(), &cqe);
        if (rc < 0) {
            std::cerr << "io_uring_wait_cqe failed: " << std::strerror(-rc) << "\n";
            return false;
        }

        int cqe_res2 = 0;
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

        const auto pending_it = pending.find(cqe->user_data);

        if (cqe->res != 0 || cqe_res2 != 0 || nvme_status_err) {
            ++errors;
            if (!opt.quiet) {
                std::uint64_t slba = 0;
                std::uint32_t nblocks = 0;
                std::uint64_t out_slba = 0;
                std::size_t buf_len = 0;
                if (pending_it != pending.end()) {
                    slba = pending_it->second.slba;
                    out_slba = pending_it->second.out_slba;
                    nblocks = pending_it->second.nblocks;
                    buf_len = pending_it->second.buf_len;
                }
                std::cerr << "[ERR] user_data=" << cqe->user_data
                          << " res=" << cqe->res
                          << " res2=" << cqe_res2
                          << " status=" << status_field
                          << " " << decode_nvme_status_field(status_field)
                          << " dw0=0x" << std::hex << dw0
                          << " dw1=0x" << dw1
                          << " dw2=0x" << dw2
                          << " dw3=0x" << dw3 << std::dec
                          << " slba=" << slba
                          << " out_slba=" << out_slba
                          << " nblocks=" << nblocks
                          << " data_len=" << buf_len
                          << "\n";
            }
        }

        if (pending_it != pending.end()) {
            free_io_buf(pending_it->second.buf);
            pending.erase(pending_it);
        }
        io_uring_cqe_seen(ring.raw(), cqe);
        if (inflight > 0) {
            --inflight;
        }
        return true;
    };

    for (const auto& job : jobs) {
        while (inflight >= opt.max_inflight) {
            if (!submit_queued()) {
                ::close(dev_fd);
                return 1;
            }
            if (!reap_one()) {
                ::close(dev_fd);
                return 1;
            }
        }

        int rc = submit_one(job);
        if (rc == -EAGAIN) {
            if (!submit_queued()) {
                ::close(dev_fd);
                return 1;
            }
            if (!reap_one()) {
                ::close(dev_fd);
                return 1;
            }
            rc = submit_one(job);
        }

        if (rc < 0) {
            std::cerr << "submit_one failed: " << std::strerror(-rc) << "\n";
            ::close(dev_fd);
            return 1;
        }

        if (queued >= submit_batch) {
            if (!submit_queued()) {
                ::close(dev_fd);
                return 1;
            }
        }
    }

    if (!submit_queued()) {
        ::close(dev_fd);
        return 1;
    }

    while (inflight > 0) {
        if (!reap_one()) {
            ::close(dev_fd);
            return 1;
        }
    }

    std::string cache_drop_err;
    if (!drop_file_cache(opt.out_path, total_bytes, &cache_drop_err) && !opt.quiet) {
        std::cerr << "[WARN] " << cache_drop_err << "\n";
    }

    const double t1 = now_us();
    if (!opt.quiet) {
        std::cout << "[INFO] segments=" << segs.size()
                  << " total_bytes=" << total_bytes
                  << " errors=" << errors
                  << " elapsed_us=" << static_cast<long long>(t1 - t0)
                  << "\n";
    }

    ::close(dev_fd);
    return (errors == 0) ? 0 : 2;
}
