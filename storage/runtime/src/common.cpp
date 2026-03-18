#include "common.h"
#include "bpe_tokenizer.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <stdexcept>
#include <ctime>
#include <sys/msg.h>
#include <sys/shm.h>
#include <utility>

namespace {

constexpr std::uint64_t kStatsMagic = 0x4250455354415431ULL; // "BPESTAT1"
constexpr std::uint32_t kArrowChunkMagic = 0x41525458U; // "ARTX"

std::uint64_t now_us() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}

struct SegmentPair {
    int id = -1;
    char* ptr = nullptr;
};

inline void stats_add_u64(std::uint64_t* p, std::uint64_t v) {
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

inline void stats_store_u64(std::uint64_t* p, std::uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

inline void stats_store_u32(std::uint32_t* p, std::uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

BpeRuntimeStats* attach_stats() {
    const int id = shmget(STATS_SHM_KEY, STATS_SHM_SIZE, IPC_CREAT | 0660);
    if (id < 0) {
        std::perror("[BPE] stats shmget");
        return nullptr;
    }
    void* ptr = shmat(id, nullptr, 0);
    if (ptr == reinterpret_cast<void*>(-1)) {
        std::perror("[BPE] stats shmat");
        return nullptr;
    }
    auto* stats = reinterpret_cast<BpeRuntimeStats*>(ptr);
    if (__atomic_load_n(&stats->magic, __ATOMIC_RELAXED) != kStatsMagic) {
        *stats = BpeRuntimeStats{};
        stats->magic = kStatsMagic;
        stats->version = 1;
        stats->start_ts_us = now_us();
    }
    return stats;
}

SegmentPair attach_slot_segment(key_t key, const char* label, std::size_t slot) {
    const int id = shmget(key, SHM_SIZE, IPC_CREAT | 0660);
    if (id < 0) {
        throw std::runtime_error(std::string(label) + " shmget failed for slot " +
                                 std::to_string(slot) + ": " + std::strerror(errno));
    }

    struct shmid_ds ds {};
    if (shmctl(id, IPC_STAT, &ds) != 0) {
        throw std::runtime_error(std::string(label) + " shmctl(IPC_STAT) failed for slot " +
                                 std::to_string(slot) + ": " + std::strerror(errno));
    }
    if (static_cast<std::size_t>(ds.shm_segsz) != SHM_SIZE) {
        throw std::runtime_error(std::string(label) + " SHM size mismatch for slot " +
                                 std::to_string(slot) + ": expected " +
                                 std::to_string(SHM_SIZE) + ", got " +
                                 std::to_string(static_cast<std::size_t>(ds.shm_segsz)));
    }

    void* ptr = shmat(id, nullptr, 0);
    if (ptr == reinterpret_cast<void*>(-1)) {
        throw std::runtime_error(std::string(label) + " shmat failed for slot " +
                                 std::to_string(slot) + ": " + std::strerror(errno));
    }

    return SegmentPair{id, static_cast<char*>(ptr)};
}

bool is_valid_utf8(std::string_view s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c <= 0x7F) {
            ++i;
            continue;
        }
        if ((c >> 5) == 0x6) {
            if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80 || c < 0xC2) {
                return false;
            }
            i += 2;
            continue;
        }
        if ((c >> 4) == 0xE) {
            if (i + 2 >= n ||
                (p[i + 1] & 0xC0) != 0x80 ||
                (p[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            if (c == 0xE0 && p[i + 1] < 0xA0) {
                return false;
            }
            if (c == 0xED && p[i + 1] >= 0xA0) {
                return false;
            }
            i += 3;
            continue;
        }
        if ((c >> 3) == 0x1E) {
            if (i + 3 >= n ||
                (p[i + 1] & 0xC0) != 0x80 ||
                (p[i + 2] & 0xC0) != 0x80 ||
                (p[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            if (c == 0xF0 && p[i + 1] < 0x90) {
                return false;
            }
            if (c == 0xF4 && p[i + 1] >= 0x90) {
                return false;
            }
            if (c > 0xF4) {
                return false;
            }
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool decode_one_utf8(std::string_view s, std::size_t pos, std::size_t* next_pos) {
    if (pos >= s.size()) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char c = p[pos];
    if (c <= 0x7F) {
        *next_pos = pos + 1;
        return true;
    }
    if ((c >> 5) == 0x6) {
        if (pos + 1 < s.size() && (p[pos + 1] & 0xC0) == 0x80 && c >= 0xC2) {
            *next_pos = pos + 2;
            return true;
        }
        return false;
    }
    if ((c >> 4) == 0xE) {
        if (pos + 2 >= s.size()) {
            return false;
        }
        if ((p[pos + 1] & 0xC0) != 0x80 || (p[pos + 2] & 0xC0) != 0x80) {
            return false;
        }
        if (c == 0xE0 && p[pos + 1] < 0xA0) {
            return false;
        }
        if (c == 0xED && p[pos + 1] >= 0xA0) {
            return false;
        }
        *next_pos = pos + 3;
        return true;
    }
    if ((c >> 3) == 0x1E) {
        if (pos + 3 >= s.size()) {
            return false;
        }
        if ((p[pos + 1] & 0xC0) != 0x80 ||
            (p[pos + 2] & 0xC0) != 0x80 ||
            (p[pos + 3] & 0xC0) != 0x80) {
            return false;
        }
        if (c == 0xF0 && p[pos + 1] < 0x90) {
            return false;
        }
        if (c == 0xF4 && p[pos + 1] >= 0x90) {
            return false;
        }
        if (c > 0xF4) {
            return false;
        }
        *next_pos = pos + 4;
        return true;
    }
    return false;
}

std::string sanitize_arrow_text_chunk(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    bool dropped_run = false;
    std::size_t i = 0;
    while (i < raw.size()) {
        std::size_t next = i;
        if (decode_one_utf8(raw, i, &next)) {
            if (dropped_run && !out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            dropped_run = false;

            const unsigned char c = static_cast<unsigned char>(raw[i]);
            if (c == 0) {
                if (!out.empty() && out.back() != ' ') {
                    out.push_back(' ');
                }
            } else {
                out.append(raw.data() + i, next - i);
            }
            i = next;
            continue;
        }

        dropped_run = true;
        ++i;
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::optional<std::string_view> parse_arrow_chunk_payload(const char* data, std::size_t len) {
    if (len < sizeof(ArrowChunkHeader)) {
        return std::nullopt;
    }
    const auto* hdr = reinterpret_cast<const ArrowChunkHeader*>(data);
    if (hdr->magic != kArrowChunkMagic || hdr->version != 1) {
        return std::nullopt;
    }
    const std::size_t off = hdr->payload_offset;
    const std::size_t plen = hdr->payload_length;
    if (off > len || plen > len || off + plen > len) {
        throw std::runtime_error("arrow chunk header has invalid payload range");
    }
    return std::string_view(data + off, plen);
}

} // namespace

ShmSlotWorker::ShmSlotWorker(std::uint32_t slot, char* read_ptr, char* write_ptr, InputMode mode)
    : slot_(slot), read_ptr_(read_ptr), write_ptr_(write_ptr), mode_(mode) {}

std::uint32_t ShmSlotWorker::Process(std::uint32_t input_len) const {
    if (read_ptr_ == nullptr || write_ptr_ == nullptr) {
        throw std::runtime_error("slot worker is not initialized");
    }

    const std::size_t safe_len = std::min<std::size_t>(input_len, SHM_SIZE);
    std::vector<std::int32_t> token_ids;
    if (mode_ == InputMode::kArrow) {
        std::string_view payload(read_ptr_, safe_len);
        if (auto framed = parse_arrow_chunk_payload(read_ptr_, safe_len); framed.has_value()) {
            payload = *framed;
        }
        const std::string sanitized = sanitize_arrow_text_chunk(payload);
        if (sanitized.empty()) {
            throw std::runtime_error(
                "arrow chunk mode found no decodable UTF-8 text; payload must be a text-buffer "
                "slice from extent-index, not a full Arrow IPC/file blob");
        }
        token_ids = Runtime::BPE::BPETokenizer::Instance().Tokenize(sanitized);
    } else {
        const std::string_view input_text(read_ptr_, safe_len);
        if (!is_valid_utf8(input_text)) {
            throw std::runtime_error("text payload is not valid UTF-8");
        }
        token_ids = Runtime::BPE::BPETokenizer::Instance().Tokenize(input_text);
    }

    auto* dest = reinterpret_cast<std::int32_t*>(write_ptr_);
    const std::size_t max_ids = SHM_SIZE / sizeof(std::int32_t);
    const std::size_t copy_count = std::min(token_ids.size(), max_ids);

    for (std::size_t i = 0; i < copy_count; ++i) {
        dest[i] = static_cast<std::int32_t>(token_ids[i]);
    }

    return static_cast<std::uint32_t>(copy_count * sizeof(std::int32_t));
}

SharedMemorySlots::SharedMemorySlots() {
    read_ids_.fill(-1);
    write_ids_.fill(-1);
    read_ptrs_.fill(nullptr);
    write_ptrs_.fill(nullptr);

    for (std::size_t s = 0; s < NUM_SLOTS; ++s) {
        try {
            const SegmentPair read_seg =
                attach_slot_segment(SHM_READ_KEY + static_cast<key_t>(s), "read", s);
            const SegmentPair write_seg =
                attach_slot_segment(SHM_WRITE_KEY + static_cast<key_t>(s), "write", s);
            read_ids_[s] = read_seg.id;
            write_ids_[s] = write_seg.id;
            read_ptrs_[s] = read_seg.ptr;
            write_ptrs_[s] = write_seg.ptr;
        } catch (...) {
            for (std::size_t i = 0; i <= s; ++i) {
                if (read_ptrs_[i] != nullptr) {
                    shmdt(read_ptrs_[i]);
                    read_ptrs_[i] = nullptr;
                }
                if (write_ptrs_[i] != nullptr) {
                    shmdt(write_ptrs_[i]);
                    write_ptrs_[i] = nullptr;
                }
                read_ids_[i] = -1;
                write_ids_[i] = -1;
            }
            throw;
        }
    }
}

SharedMemorySlots::~SharedMemorySlots() {
    for (std::size_t s = 0; s < NUM_SLOTS; ++s) {
        if (read_ptrs_[s] != nullptr) {
            shmdt(read_ptrs_[s]);
            read_ptrs_[s] = nullptr;
        }
        if (write_ptrs_[s] != nullptr) {
            shmdt(write_ptrs_[s]);
            write_ptrs_[s] = nullptr;
        }
    }
}

std::vector<ShmSlotWorker> SharedMemorySlots::CreateWorkers(InputMode mode) const {
    std::vector<ShmSlotWorker> workers;
    workers.reserve(NUM_SLOTS);
    for (std::size_t s = 0; s < NUM_SLOTS; ++s) {
        workers.emplace_back(static_cast<std::uint32_t>(s), read_ptrs_[s], write_ptrs_[s], mode);
    }
    return workers;
}

MessageQueueDispatcher::MessageQueueDispatcher(int msg_id, std::vector<ShmSlotWorker> workers)
    : msg_id_(msg_id), workers_(std::move(workers)) {}

int MessageQueueDispatcher::OpenQueue(key_t key, int flags) {
    return msgget(key, flags);
}

bool MessageQueueDispatcher::Receive(bpe_msg_req& req) const {
    const ssize_t n = msgrcv(msg_id_, &req, BPE_REQ_MSZ, 1, 0);
    if (n < 0) {
        std::perror("[BPE] msgrcv");
        return false;
    }
    return true;
}

void MessageQueueDispatcher::Send(const bpe_msg_resp& resp) const {
    if (msgsnd(msg_id_, &resp, BPE_RESP_MSZ, 0) < 0) {
        std::perror("[BPE] msgsnd");
    }
}

void MessageQueueDispatcher::Run() const {
    BpeRuntimeStats* stats = attach_stats();
    for (;;) {
        bpe_msg_req req{};
        if (!Receive(req)) {
            continue;
        }

        const std::uint64_t t0 = now_us();
        if (stats) {
            stats_store_u64(&stats->last_ts_us, t0);
            stats_store_u64(&stats->last_req_id, req.req_id);
            stats_store_u32(&stats->last_slot, req.slot);
            stats_add_u64(&stats->req_total, 1);
            stats_add_u64(&stats->bytes_in, req.total_len);
            if (req.slot < NUM_SLOTS) {
                stats_add_u64(&stats->per_slot_req[req.slot], 1);
                stats_add_u64(&stats->per_slot_bytes_in[req.slot], req.total_len);
            }
        }

        bpe_msg_resp resp{};
        resp.msg_type = 2;
        resp.req_id = req.req_id;
        resp.slot = req.slot;

        if (req.slot >= workers_.size()) {
            resp.byte_size = 0;
            Send(resp);
            continue;
        }

        const std::uint32_t total_len = std::min<std::uint32_t>(req.total_len, SHM_SIZE);
        resp.byte_size = workers_[req.slot].Process(total_len);
        const std::uint64_t t1 = now_us();
        if (stats) {
            stats_store_u64(&stats->last_ts_us, t1);
            stats_store_u64(&stats->last_latency_us, (t1 >= t0) ? (t1 - t0) : 0);
            stats_store_u32(&stats->last_resp_bytes, resp.byte_size);
            stats_add_u64(&stats->resp_total, 1);
            stats_add_u64(&stats->bytes_out, resp.byte_size);
            if (req.slot < NUM_SLOTS) {
                stats_add_u64(&stats->per_slot_resp[req.slot], 1);
                stats_add_u64(&stats->per_slot_bytes_out[req.slot], resp.byte_size);
            }
        }
        Send(resp);
    }
}
