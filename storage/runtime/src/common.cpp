#include "common.h"
#include "bpe_tokenizer.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/eventfd.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr std::uint64_t kStatsMagic = 0x4250455354415431ULL; // "BPESTAT1"
constexpr std::uint64_t kIpcConfigMagic = 0x4250454346473031ULL; // "BPECFG01"
constexpr std::uint64_t kIpcStateMagic = 0x4250455354415445ULL; // "BPESTATE"

struct SegmentPair {
    int id = -1;
    char* ptr = nullptr;
};

struct SlotEventFds {
    int req_fd = -1;
    int cpl_fd = -1;
};

struct EventfdHandshakeMsg {
    std::uint32_t slot = 0;
    std::uint32_t reserved = 0;
};

std::uint64_t now_us() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}

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

BpeRuntimeIpcConfig* attach_ipc_config() {
    const int id = shmget(IPC_CONFIG_SHM_KEY, IPC_CONFIG_SHM_SIZE, IPC_CREAT | 0660);
    if (id < 0) {
        std::perror("[BPE] config shmget");
        return nullptr;
    }
    void* ptr = shmat(id, nullptr, 0);
    if (ptr == reinterpret_cast<void*>(-1)) {
        std::perror("[BPE] config shmat");
        return nullptr;
    }
    auto* cfg = reinterpret_cast<BpeRuntimeIpcConfig*>(ptr);
    if (__atomic_load_n(&cfg->magic, __ATOMIC_RELAXED) != kIpcConfigMagic) {
        *cfg = BpeRuntimeIpcConfig{};
        cfg->magic = kIpcConfigMagic;
        cfg->version = 1;
    }
    return cfg;
}

BpeRuntimeIpcState* attach_ipc_state() {
    const int id = shmget(IPC_STATE_SHM_KEY, IPC_STATE_SHM_SIZE, IPC_CREAT | 0660);
    if (id < 0) {
        std::perror("[BPE] state shmget");
        return nullptr;
    }
    void* ptr = shmat(id, nullptr, 0);
    if (ptr == reinterpret_cast<void*>(-1)) {
        std::perror("[BPE] state shmat");
        return nullptr;
    }
    auto* state = reinterpret_cast<BpeRuntimeIpcState*>(ptr);
    if (__atomic_load_n(&state->magic, __ATOMIC_RELAXED) != kIpcStateMagic) {
        *state = BpeRuntimeIpcState{};
        state->magic = kIpcStateMagic;
        state->version = 1;
    }
    return state;
}

void reset_ipc_state(BpeRuntimeIpcState* state) {
    if (state == nullptr) {
        return;
    }
    *state = BpeRuntimeIpcState{};
    state->magic = kIpcStateMagic;
    state->version = 1;
}

key_t slot_segment_key(bool is_read, std::size_t slot, std::size_t bank_index) {
    const key_t base = is_read ? SHM_READ_KEY : SHM_WRITE_KEY;
    return base + static_cast<key_t>(bank_index) * SHM_BANK_STRIDE +
           static_cast<key_t>(slot);
}

SegmentPair attach_slot_segment(key_t key, std::size_t segment_size, const char* label,
                                std::size_t slot, std::size_t bank_index) {
    const int id = shmget(key, segment_size, IPC_CREAT | 0660);
    if (id < 0) {
        throw std::runtime_error(std::string(label) + " shmget failed for slot " +
                                 std::to_string(slot) + " bank " +
                                 std::to_string(bank_index) + ": " + std::strerror(errno));
    }

    struct shmid_ds ds {};
    if (shmctl(id, IPC_STAT, &ds) != 0) {
        throw std::runtime_error(std::string(label) + " shmctl(IPC_STAT) failed for slot " +
                                 std::to_string(slot) + " bank " +
                                 std::to_string(bank_index) + ": " + std::strerror(errno));
    }
    if (static_cast<std::size_t>(ds.shm_segsz) != segment_size) {
        throw std::runtime_error(std::string(label) + " SHM size mismatch for slot " +
                                 std::to_string(slot) + " bank " +
                                 std::to_string(bank_index) + ": expected " +
                                 std::to_string(segment_size) + ", got " +
                                 std::to_string(static_cast<std::size_t>(ds.shm_segsz)));
    }

    void* ptr = shmat(id, nullptr, 0);
    if (ptr == reinterpret_cast<void*>(-1)) {
        throw std::runtime_error(std::string(label) + " shmat failed for slot " +
                                 std::to_string(slot) + " bank " +
                                 std::to_string(bank_index) + ": " + std::strerror(errno));
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

std::string preview_text_for_log(std::string_view raw, std::size_t limit = 64) {
    const std::size_t n = std::min(raw.size(), limit);
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\n' || c == '\r' || c == '\t') {
            out.push_back(' ');
        } else if (c < 0x20 || c == 0x7F) {
            out.push_back('?');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

constexpr std::uint32_t kNdtPackedPayloadMagic = 0x31504b4eU; // "NKP1"
constexpr std::uint32_t kNdtPackedPayloadVersion = 1U;
constexpr std::size_t kNdtPackedPayloadHeaderBytes = 16U;

std::uint32_t read_u32_le(const char* p) {
    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(p[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(p[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(p[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
    return b0 | (b1 << 8U) | (b2 << 16U) | (b3 << 24U);
}

bool is_ndt_packed_payload(std::string_view payload) {
    if (payload.size() < kNdtPackedPayloadHeaderBytes) {
        return false;
    }
    return read_u32_le(payload.data()) == kNdtPackedPayloadMagic;
}

std::vector<std::int32_t> tokenize_pure_text_payload(std::string_view input_text) {
    if (input_text.empty()) {
        return {};
    }
    if (!is_valid_utf8(input_text)) {
        throw std::runtime_error("pure text payload is not valid UTF-8");
    }
    return Runtime::BPE::BPETokenizer::Instance().Tokenize(input_text);
}

std::vector<std::int32_t> tokenize_ndt_packed_payload(std::string_view payload,
                                                      std::size_t* record_count_out) {
    if (payload.size() < kNdtPackedPayloadHeaderBytes) {
        throw std::runtime_error("NDT packed payload is shorter than header");
    }
    const std::uint32_t magic = read_u32_le(payload.data());
    const std::uint32_t version = read_u32_le(payload.data() + 4);
    const std::uint32_t record_count = read_u32_le(payload.data() + 8);
    const std::uint32_t data_offset = read_u32_le(payload.data() + 12);
    if (magic != kNdtPackedPayloadMagic) {
        throw std::runtime_error("invalid NDT packed payload magic");
    }
    if (version != kNdtPackedPayloadVersion) {
        throw std::runtime_error("unsupported NDT packed payload version");
    }
    if (record_count == 0) {
        if (record_count_out != nullptr) {
            *record_count_out = 0;
        }
        return {};
    }
    const std::uint64_t expected_data_offset =
        kNdtPackedPayloadHeaderBytes +
        static_cast<std::uint64_t>(record_count) * sizeof(std::uint32_t);
    if (data_offset != expected_data_offset || data_offset > payload.size()) {
        throw std::runtime_error("invalid NDT packed payload data offset");
    }

    std::vector<std::int32_t> all_ids;
    std::size_t pos = data_offset;
    for (std::uint32_t i = 0; i < record_count; ++i) {
        const std::size_t len_off =
            kNdtPackedPayloadHeaderBytes +
            static_cast<std::size_t>(i) * sizeof(std::uint32_t);
        const std::uint32_t record_len = read_u32_le(payload.data() + len_off);
        if (record_len > payload.size() - pos) {
            throw std::runtime_error("NDT packed payload record length exceeds payload");
        }
        const std::string_view record(payload.data() + pos, record_len);
        if (!record.empty()) {
            if (!is_valid_utf8(record)) {
                throw std::runtime_error("NDT packed record is not valid UTF-8");
            }
            auto ids = Runtime::BPE::BPETokenizer::Instance().Tokenize(record);
            all_ids.insert(all_ids.end(), ids.begin(), ids.end());
        }
        pos += record_len;
    }
    if (pos != payload.size()) {
        throw std::runtime_error("NDT packed payload has trailing bytes");
    }
    if (record_count_out != nullptr) {
        *record_count_out = record_count;
    }
    return all_ids;
}

std::size_t default_exec_workers() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1, std::min<std::size_t>(NUM_SLOTS, hw == 0 ? 1 : hw));
}

std::size_t resolved_exec_workers(const MessageQueueDispatcher::Options& options) {
    if (options.exec_mode == ExecMode::kInline) {
        return 1;
    }
    return options.workers == 0 ? default_exec_workers()
                                : std::max<std::size_t>(1, std::min<std::size_t>(NUM_SLOTS, options.workers));
}

void publish_ipc_config(BpeRuntimeIpcConfig* cfg,
                        std::size_t request_workers,
                        ExecMode exec_mode) {
    if (cfg == nullptr) {
        return;
    }
    __atomic_store_n(&cfg->request_workers,
                     static_cast<std::uint32_t>(request_workers),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&cfg->exec_mode,
                     static_cast<std::uint32_t>(exec_mode),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&cfg->version, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&cfg->magic, kIpcConfigMagic, __ATOMIC_RELEASE);
}

void record_request_stats(BpeRuntimeStats* stats, const bpe_msg_req& req, std::uint64_t t0) {
    if (stats == nullptr) {
        return;
    }
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

void record_response_stats(BpeRuntimeStats* stats,
                           const bpe_msg_req& req,
                           std::uint32_t resp_bytes,
                           std::uint64_t t0,
                           std::uint64_t t1) {
    if (stats == nullptr) {
        return;
    }
    stats_store_u64(&stats->last_ts_us, t1);
    stats_store_u64(&stats->last_latency_us, (t1 >= t0) ? (t1 - t0) : 0);
    stats_store_u32(&stats->last_resp_bytes, resp_bytes);
    stats_add_u64(&stats->resp_total, 1);
    stats_add_u64(&stats->bytes_out, resp_bytes);
    if (req.slot < NUM_SLOTS) {
        stats_add_u64(&stats->per_slot_resp[req.slot], 1);
        stats_add_u64(&stats->per_slot_bytes_out[req.slot], resp_bytes);
    }
}

std::uint32_t process_request_bytes(const std::vector<ShmSlotWorker>& workers,
                                    std::size_t slot,
                                    std::size_t bank_index,
                                    const bpe_msg_req& req) {
    if (slot >= workers.size()) {
        return BPE_OUTPUT_ERROR;
    }
    const std::uint32_t total_len = std::min<std::uint32_t>(req.total_len, INPUT_SHM_SIZE);
    try {
        return workers[slot].Process(bank_index, total_len);
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "[BPE] slot=%zu bank=%zu req_id=%llu failed: %s\n",
                     slot,
                     bank_index,
                     static_cast<unsigned long long>(req.req_id),
                     ex.what());
        return BPE_OUTPUT_ERROR;
    }
}

bool signal_eventfd(int fd) {
    const std::uint64_t one = 1;
    for (;;) {
        const ssize_t rc = ::write(fd, &one, sizeof(one));
        if (rc == static_cast<ssize_t>(sizeof(one))) {
            return true;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        std::perror("[BPE] eventfd write");
        return false;
    }
}

void drain_eventfd_counter(int fd) {
    std::uint64_t value = 0;
    for (;;) {
        const ssize_t rc = ::read(fd, &value, sizeof(value));
        if (rc == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0 && errno == EAGAIN) {
            return;
        }
        if (rc == 0) {
            return;
        }
        std::perror("[BPE] eventfd read");
        return;
    }
}

bool send_fds(int sock_fd, const EventfdHandshakeMsg& msg, const int* fds, std::size_t fd_count) {
    char control[CMSG_SPACE(sizeof(int) * 2)] = {};
    struct iovec iov {
        .iov_base = const_cast<EventfdHandshakeMsg*>(&msg),
        .iov_len = sizeof(msg),
    };
    struct msghdr hdr {};
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = control;
    hdr.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);

    for (;;) {
        const ssize_t rc = sendmsg(sock_fd, &hdr, 0);
        if (rc == static_cast<ssize_t>(sizeof(msg))) {
            return true;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        std::perror("[BPE] sendmsg");
        return false;
    }
}

void publish_eventfds_and_wait_for_peer(const std::array<SlotEventFds, NUM_SLOTS>& slot_fds) {
    const int server_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (server_fd < 0) {
        throw std::runtime_error(std::string("socket(AF_UNIX) failed: ") + std::strerror(errno));
    }

    ::unlink(EVENTFD_SOCKET_PATH);

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", EVENTFD_SOCKET_PATH);
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int saved_errno = errno;
        ::close(server_fd);
        throw std::runtime_error(std::string("bind(") + EVENTFD_SOCKET_PATH + ") failed: " +
                                 std::strerror(saved_errno));
    }
    if (listen(server_fd, 1) != 0) {
        const int saved_errno = errno;
        ::close(server_fd);
        throw std::runtime_error(std::string("listen(") + EVENTFD_SOCKET_PATH + ") failed: " +
                                 std::strerror(saved_errno));
    }

    std::fprintf(stderr, "[BPE] waiting for SPDK eventfd handshake on %s\n", EVENTFD_SOCKET_PATH);

    const int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
        const int saved_errno = errno;
        ::close(server_fd);
        throw std::runtime_error(std::string("accept(") + EVENTFD_SOCKET_PATH + ") failed: " +
                                 std::strerror(saved_errno));
    }

    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        const EventfdHandshakeMsg msg {
            static_cast<std::uint32_t>(slot),
            0,
        };
        const int fds[2] = {
            slot_fds[slot].req_fd,
            slot_fds[slot].cpl_fd,
        };
        if (!send_fds(client_fd, msg, fds, 2)) {
            ::close(client_fd);
            ::close(server_fd);
            throw std::runtime_error("failed to publish eventfds to SPDK");
        }
    }

    ::close(client_fd);
    ::close(server_fd);
}

void start_eventfd_handshake_server_async(const std::array<SlotEventFds, NUM_SLOTS>& slot_fds) {
    std::thread([slot_fds]() {
        try {
            publish_eventfds_and_wait_for_peer(slot_fds);
            std::fprintf(stderr,
                         "[BPE] SPDK eventfd handshake completed on %s\n",
                         EVENTFD_SOCKET_PATH);
        } catch (const std::exception& ex) {
            std::fprintf(stderr,
                         "[BPE] SPDK eventfd handshake failed: %s\n",
                         ex.what());
        }
    }).detach();
}

std::array<SlotEventFds, NUM_SLOTS> create_slot_eventfds() {
    std::array<SlotEventFds, NUM_SLOTS> slot_fds {};
    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        slot_fds[slot].req_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        slot_fds[slot].cpl_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (slot_fds[slot].req_fd < 0 || slot_fds[slot].cpl_fd < 0) {
            throw std::runtime_error(std::string("eventfd creation failed for slot ") +
                                     std::to_string(slot) + ": " + std::strerror(errno));
        }
    }
    return slot_fds;
}

std::vector<std::size_t> owned_slots_for_worker(std::size_t worker_index, std::size_t worker_count) {
    std::vector<std::size_t> slots;
    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        if (slot % worker_count == worker_index) {
            slots.push_back(slot);
        }
    }
    return slots;
}

BpeSlotBufferState* slot_buffer_state(BpeRuntimeIpcState* state, std::size_t slot, std::size_t bank_index) {
    return &state->slots[slot].buffers[bank_index];
}

bool try_claim_ready_buffer(BpeRuntimeIpcState* state,
                            std::size_t slot,
                            std::size_t bank_index,
                            bpe_msg_req* req) {
    auto* buffer = slot_buffer_state(state, slot, bank_index);
    std::uint32_t expected = static_cast<std::uint32_t>(BpeBufferState::kReady);
    if (!__atomic_compare_exchange_n(&buffer->state,
                                     &expected,
                                     static_cast<std::uint32_t>(BpeBufferState::kProcessing),
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return false;
    }

    __sync_synchronize();

    req->req_id = __atomic_load_n(&buffer->req_id, __ATOMIC_ACQUIRE);
    req->total_len = __atomic_load_n(&buffer->input_len, __ATOMIC_ACQUIRE);
    req->slot = static_cast<std::uint32_t>(slot);
    return true;
}

void mark_buffer_done(BpeRuntimeIpcState* state,
                      std::size_t slot,
                      std::size_t bank_index,
                      std::uint32_t output_len) {
    auto* buffer = slot_buffer_state(state, slot, bank_index);
    __atomic_store_n(&buffer->output_len, output_len, __ATOMIC_RELEASE);
    __sync_synchronize();
    __atomic_store_n(&buffer->state,
                     static_cast<std::uint32_t>(BpeBufferState::kDone),
                     __ATOMIC_RELEASE);
}

void drain_slot_ready_buffers(std::size_t slot,
                              const SlotEventFds& slot_fds,
                              const std::vector<ShmSlotWorker>& workers,
                              BpeRuntimeIpcState* ipc_state,
                              BpeRuntimeStats* stats) {
    for (;;) {
        bool did_work = false;
        for (std::size_t bank_index = 0; bank_index < NUM_BUFFERS; ++bank_index) {
            bpe_msg_req req {};
            if (!try_claim_ready_buffer(ipc_state, slot, bank_index, &req)) {
                continue;
            }

            const std::uint64_t t0 = now_us();
            record_request_stats(stats, req, t0);
            std::fprintf(stderr, "[BPE] runtime recv slot=%zu bank=%zu req_id=%llu input_len=%u\n",
                         slot,
                         bank_index,
                         static_cast<unsigned long long>(req.req_id),
                         req.total_len);
            const std::uint64_t process_start_us = now_us();
            const std::uint32_t output_len =
                process_request_bytes(workers, slot, bank_index, req);
            const std::uint64_t process_done_us = now_us();
            std::fprintf(stderr, "[BPE] runtime done slot=%zu bank=%zu req_id=%llu input_len=%u output_len=%u\n",
                         slot,
                         bank_index,
                         static_cast<unsigned long long>(req.req_id),
                         req.total_len,
                         output_len);
            const std::uint64_t t1 = now_us();
            const std::uint32_t stats_len =
                (output_len == BPE_OUTPUT_ERROR) ? 0 : output_len;
            record_response_stats(stats, req, stats_len, t0, t1);
            const std::uint64_t mark_start_us = now_us();
            mark_buffer_done(ipc_state, slot, bank_index, output_len);
            const std::uint64_t mark_done_us = now_us();
            (void)signal_eventfd(slot_fds.cpl_fd);
            const std::uint64_t signal_done_us = now_us();
            std::fprintf(stderr,
                         "[BPE_RUNTIME_BREAKDOWN] req_id=%llu slot=%zu bank=%zu "
                         "claim_to_process_us=%llu process_request_us=%llu "
                         "stats_us=%llu mark_done_us=%llu completion_eventfd_us=%llu "
                         "runtime_total_us=%llu input_len=%u output_len=%u\n",
                         static_cast<unsigned long long>(req.req_id),
                         slot,
                         bank_index,
                         static_cast<unsigned long long>(process_start_us - t0),
                         static_cast<unsigned long long>(process_done_us - process_start_us),
                         static_cast<unsigned long long>(t1 - process_done_us),
                         static_cast<unsigned long long>(mark_done_us - mark_start_us),
                         static_cast<unsigned long long>(signal_done_us - mark_done_us),
                         static_cast<unsigned long long>(signal_done_us - t0),
                         req.total_len,
                         output_len);
            did_work = true;
        }
        if (!did_work) {
            return;
        }
    }
}

void run_worker_loop(std::size_t worker_index,
                     std::size_t worker_count,
                     const std::array<SlotEventFds, NUM_SLOTS>* slot_fds,
                     const std::vector<ShmSlotWorker>* workers,
                     BpeRuntimeIpcState* ipc_state,
                     BpeRuntimeStats* stats) {
    const std::vector<std::size_t> owned_slots = owned_slots_for_worker(worker_index, worker_count);
    std::vector<struct pollfd> poll_fds;
    poll_fds.reserve(owned_slots.size());
    for (const std::size_t slot : owned_slots) {
        poll_fds.push_back(pollfd {
            .fd = (*slot_fds)[slot].req_fd,
            .events = POLLIN,
            .revents = 0,
        });
    }

    for (;;) {
        const int rc = poll(poll_fds.data(), poll_fds.size(), -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("[BPE] poll");
            continue;
        }

        for (std::size_t i = 0; i < poll_fds.size(); ++i) {
            if ((poll_fds[i].revents & POLLIN) == 0) {
                continue;
            }
            drain_eventfd_counter(poll_fds[i].fd);
            drain_slot_ready_buffers(owned_slots[i], (*slot_fds)[owned_slots[i]], *workers, ipc_state, stats);
            poll_fds[i].revents = 0;
        }
    }
}

} // namespace

ShmSlotWorker::ShmSlotWorker(std::uint32_t slot,
                             BankPtrs read_ptrs,
                             BankPtrs write_ptrs,
                             InputMode mode)
    : slot_(slot),
      read_ptrs_(read_ptrs),
      write_ptrs_(write_ptrs),
      mode_(mode) {}

std::uint32_t ShmSlotWorker::Process(std::size_t bank_index, std::uint32_t input_len) const {
    if (bank_index >= NUM_BUFFERS) {
        throw std::runtime_error("invalid bank index");
    }
    if (read_ptrs_[bank_index] == nullptr || write_ptrs_[bank_index] == nullptr) {
        throw std::runtime_error("slot worker is not initialized");
    }

    const std::size_t safe_len = std::min<std::size_t>(input_len, INPUT_SHM_SIZE);
    std::vector<std::int32_t> token_ids;
    const std::uint64_t process_start_us = now_us();
    std::uint64_t parse_done_us = process_start_us;
    std::uint64_t tokenize_done_us = process_start_us;
    std::uint64_t copy_done_us = process_start_us;
    {
        const std::string_view input_text(read_ptrs_[bank_index], safe_len);
        parse_done_us = now_us();
        const bool packed = is_ndt_packed_payload(input_text);
        const bool utf8_ok = packed ? true : is_valid_utf8(input_text);
        std::size_t packed_records = 0;
        std::fprintf(stderr,
                     "[BPE] tokenize slot=%u bank=%zu mode=%s raw_len=%zu utf8=%s preview=\"%s\"\n",
                     slot_,
                     bank_index,
                     packed ? "ndt_packed" : "pure_text",
                     safe_len,
                     utf8_ok ? "yes" : "no",
                     preview_text_for_log(input_text).c_str());
        const std::uint64_t tokenize_start_us = now_us();
        if (packed) {
            token_ids = tokenize_ndt_packed_payload(input_text, &packed_records);
        } else {
            token_ids = tokenize_pure_text_payload(input_text);
        }
        tokenize_done_us = now_us();
        parse_done_us = tokenize_start_us;
        if (packed) {
            std::fprintf(stderr,
                         "[BPE] packed-records slot=%u bank=%zu records=%zu\n",
                         slot_,
                         bank_index,
                         packed_records);
        }
    }

    auto* dest = reinterpret_cast<std::int32_t*>(write_ptrs_[bank_index]);
    const std::size_t max_ids = OUTPUT_SHM_SIZE / sizeof(std::int32_t);
    if (token_ids.size() > max_ids) {
        throw std::runtime_error("BPE output exceeds overflow-safe output bank");
    }
    const std::size_t copy_count = token_ids.size();
    if (copy_count > 0) {
        const std::size_t sample = std::min<std::size_t>(copy_count, 8);
        std::fprintf(stderr,
                     "[BPE] tokenize-sample slot=%u bank=%zu",
                     slot_,
                     bank_index);
        for (std::size_t i = 0; i < sample; ++i) {
            std::fprintf(stderr, " %d", token_ids[i]);
        }
        std::fprintf(stderr, "\n");
    }
    if (copy_count > 0) {
        std::memcpy(dest, token_ids.data(), copy_count * sizeof(std::int32_t));
    }
    copy_done_us = now_us();

    const std::uint32_t output_len = static_cast<std::uint32_t>(copy_count * sizeof(std::int32_t));
    std::fprintf(stderr,
                 "[BPE_RUNTIME_STAGE] slot=%u bank=%zu "
                 "input_prepare_us=%llu tokenize_us=%llu output_copy_us=%llu "
                 "process_total_us=%llu tokens=%zu output_bytes=%u\n",
                 slot_,
                 bank_index,
                 static_cast<unsigned long long>(parse_done_us - process_start_us),
                 static_cast<unsigned long long>(tokenize_done_us - parse_done_us),
                 static_cast<unsigned long long>(copy_done_us - tokenize_done_us),
                 static_cast<unsigned long long>(copy_done_us - process_start_us),
                 copy_count,
                 output_len);
    std::fprintf(stderr,
                 "[BPE] tokenize-done slot=%u bank=%zu tokens=%zu output_bytes=%u\n",
                 slot_,
                 bank_index,
                 copy_count,
                 output_len);
    return output_len;
}

SharedMemorySlots::SharedMemorySlots() {
    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        read_ids_[slot].fill(-1);
        write_ids_[slot].fill(-1);
        read_ptrs_[slot].fill(nullptr);
        write_ptrs_[slot].fill(nullptr);
    }

    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        try {
            for (std::size_t bank_index = 0; bank_index < NUM_BUFFERS; ++bank_index) {
                const SegmentPair read_seg =
                    attach_slot_segment(slot_segment_key(true, slot, bank_index), INPUT_SHM_SIZE,
                                        "read", slot, bank_index);
                const SegmentPair write_seg =
                    attach_slot_segment(slot_segment_key(false, slot, bank_index), OUTPUT_SHM_SIZE,
                                        "write", slot, bank_index);
                read_ids_[slot][bank_index] = read_seg.id;
                write_ids_[slot][bank_index] = write_seg.id;
                read_ptrs_[slot][bank_index] = read_seg.ptr;
                write_ptrs_[slot][bank_index] = write_seg.ptr;
            }
        } catch (...) {
            for (std::size_t i = 0; i <= slot; ++i) {
                for (std::size_t bank_index = 0; bank_index < NUM_BUFFERS; ++bank_index) {
                    if (read_ptrs_[i][bank_index] != nullptr) {
                        shmdt(read_ptrs_[i][bank_index]);
                        read_ptrs_[i][bank_index] = nullptr;
                    }
                    if (write_ptrs_[i][bank_index] != nullptr) {
                        shmdt(write_ptrs_[i][bank_index]);
                        write_ptrs_[i][bank_index] = nullptr;
                    }
                    read_ids_[i][bank_index] = -1;
                    write_ids_[i][bank_index] = -1;
                }
            }
            throw;
        }
    }
}

SharedMemorySlots::~SharedMemorySlots() {
    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        for (std::size_t bank_index = 0; bank_index < NUM_BUFFERS; ++bank_index) {
            if (read_ptrs_[slot][bank_index] != nullptr) {
                shmdt(read_ptrs_[slot][bank_index]);
                read_ptrs_[slot][bank_index] = nullptr;
            }
            if (write_ptrs_[slot][bank_index] != nullptr) {
                shmdt(write_ptrs_[slot][bank_index]);
                write_ptrs_[slot][bank_index] = nullptr;
            }
        }
    }
}

std::vector<ShmSlotWorker> SharedMemorySlots::CreateWorkers(InputMode mode) const {
    std::vector<ShmSlotWorker> workers;
    workers.reserve(NUM_SLOTS);
    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        workers.emplace_back(static_cast<std::uint32_t>(slot),
                             read_ptrs_[slot],
                             write_ptrs_[slot],
                             mode);
    }
    return workers;
}

MessageQueueDispatcher::MessageQueueDispatcher(std::vector<ShmSlotWorker> workers)
    : MessageQueueDispatcher(std::move(workers), Options{}) {}

MessageQueueDispatcher::MessageQueueDispatcher(std::vector<ShmSlotWorker> workers, Options options)
    : workers_(std::move(workers)),
      options_(options) {}

void MessageQueueDispatcher::Run() const {
    BpeRuntimeStats* stats = attach_stats();
    BpeRuntimeIpcConfig* ipc_cfg = attach_ipc_config();
    BpeRuntimeIpcState* ipc_state = attach_ipc_state();
    const std::size_t exec_workers = resolved_exec_workers(options_);

    reset_ipc_state(ipc_state);
    publish_ipc_config(ipc_cfg, exec_workers, options_.exec_mode);

    const std::array<SlotEventFds, NUM_SLOTS> slot_fds = create_slot_eventfds();
    start_eventfd_handshake_server_async(slot_fds);

    if (options_.exec_mode == ExecMode::kThread) {
        std::vector<std::thread> threads;
        threads.reserve(exec_workers);
        for (std::size_t worker_index = 0; worker_index < exec_workers; ++worker_index) {
            threads.emplace_back(run_worker_loop,
                                 worker_index,
                                 exec_workers,
                                 &slot_fds,
                                 &workers_,
                                 ipc_state,
                                 stats);
        }
        for (auto& thread : threads) {
            thread.join();
        }
        return;
    }

    if (options_.exec_mode == ExecMode::kProcess) {
        for (std::size_t worker_index = 0; worker_index < exec_workers; ++worker_index) {
            const pid_t pid = ::fork();
            if (pid < 0) {
                throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
            }

            if (pid == 0) {
                run_worker_loop(worker_index, exec_workers, &slot_fds, &workers_, ipc_state, stats);
                std::_Exit(0);
            }
        }
        for (;;) {
            int status = 0;
            const pid_t child = ::wait(&status);
            if (child < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::perror("[BPE] wait");
                return;
            }
            std::fprintf(stderr, "[BPE] worker process %d exited with status=%d\n", child, status);
        }
    }

    run_worker_loop(0, 1, &slot_fds, &workers_, ipc_state, stats);
}
