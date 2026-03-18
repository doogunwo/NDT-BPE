#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sys/ipc.h>

constexpr key_t SHM_READ_KEY = 0x1000;
constexpr key_t SHM_WRITE_KEY = 0x1022;
constexpr key_t MSG_KEY = 1002;
constexpr std::size_t SHM_SIZE = 131072; // 128 KiB
constexpr std::size_t NUM_SLOTS = 16;
constexpr key_t STATS_SHM_KEY = 0x2000;
constexpr std::size_t STATS_SHM_SIZE = 4096;

struct BpeRuntimeStats {
    std::uint64_t magic = 0;
    std::uint32_t version = 1;
    std::uint32_t reserved = 0;

    std::uint64_t start_ts_us = 0;
    std::uint64_t last_ts_us = 0;
    std::uint64_t last_latency_us = 0;
    std::uint64_t last_req_id = 0;
    std::uint32_t last_slot = 0;
    std::uint32_t last_resp_bytes = 0;

    std::uint64_t req_total = 0;
    std::uint64_t resp_total = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;

    std::array<std::uint64_t, NUM_SLOTS> per_slot_req{};
    std::array<std::uint64_t, NUM_SLOTS> per_slot_resp{};
    std::array<std::uint64_t, NUM_SLOTS> per_slot_bytes_in{};
    std::array<std::uint64_t, NUM_SLOTS> per_slot_bytes_out{};
};

static_assert(sizeof(BpeRuntimeStats) <= STATS_SHM_SIZE,
              "BpeRuntimeStats no longer fits in the stats shared memory segment");

struct __attribute__((packed)) bpe_msg_req {
    long msg_type;      // == 1
    std::uint32_t total_len;
    std::uint64_t req_id;
    std::uint32_t slot; // cdw13
};

struct __attribute__((packed)) bpe_msg_resp {
    long msg_type; // == 2
    std::uint32_t byte_size;
    std::uint64_t req_id;
    std::uint32_t slot;
};

enum {
    BPE_REQ_MSZ = static_cast<int>(sizeof(bpe_msg_req) - sizeof(long)),
    BPE_RESP_MSZ = static_cast<int>(sizeof(bpe_msg_resp) - sizeof(long)),
};

enum class InputMode : std::uint32_t {
    kText = 0,
    kArrow = 1,
};

// Arrow mode does not receive a full Arrow IPC/file payload.
// It receives bytes from the Arrow text values buffer path described by
// `compute/src/extent-index.cpp`. Optional framing allows the producer to
// identify the valid subrange inside an aligned storage chunk.
struct __attribute__((packed)) ArrowChunkHeader {
    std::uint32_t magic = 0x41525458U; // "ARTX"
    std::uint16_t version = 1;
    std::uint16_t reserved = 0;
    std::uint32_t payload_offset = 0;
    std::uint32_t payload_length = 0;
    std::uint64_t data_range_offset = 0;
    std::int64_t batch_index = -1;
    std::int64_t num_rows = -1;
};

class ShmSlotWorker {
public:
    ShmSlotWorker() = default;
    ShmSlotWorker(std::uint32_t slot, char* read_ptr, char* write_ptr, InputMode mode);

    std::uint32_t Process(std::uint32_t input_len) const;
    std::uint32_t slot() const { return slot_; }

private:
    std::uint32_t slot_ = 0;
    char* read_ptr_ = nullptr;
    char* write_ptr_ = nullptr;
    InputMode mode_ = InputMode::kText;
};

class SharedMemorySlots {
public:
    SharedMemorySlots();
    ~SharedMemorySlots();

    SharedMemorySlots(const SharedMemorySlots&) = delete;
    SharedMemorySlots& operator=(const SharedMemorySlots&) = delete;

    std::vector<ShmSlotWorker> CreateWorkers(InputMode mode) const;
    std::vector<ShmSlotWorker> CreateWorkers() const { return CreateWorkers(InputMode::kText); }

private:
    std::array<int, NUM_SLOTS> read_ids_{};
    std::array<int, NUM_SLOTS> write_ids_{};
    std::array<char*, NUM_SLOTS> read_ptrs_{};
    std::array<char*, NUM_SLOTS> write_ptrs_{};
};

class MessageQueueDispatcher {
public:
    MessageQueueDispatcher(int msg_id, std::vector<ShmSlotWorker> workers);

    static int OpenQueue(key_t key = MSG_KEY, int flags = IPC_CREAT | 0660);
    void Run() const;

private:
    bool Receive(bpe_msg_req& req) const;
    void Send(const bpe_msg_resp& resp) const;

    int msg_id_ = -1;
    std::vector<ShmSlotWorker> workers_;
};
